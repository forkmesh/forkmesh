import Foundation

public struct CallFaceAxisFrame: Equatable, Sendable {
    public let center: CallFacePoint
    public let x: CallFacePoint
    public let y: CallFacePoint
}

public struct CallFaceHairRegion: Equatable, Sendable {
    public let center: CallFacePoint
    public let size: CallFacePoint
}

public enum CallFaceWarp {
    public static let vertexFloats = 7
    public static let hairSide = 6.4
    public static let hairCenter = CallFacePoint(0, 0.5, -0.3)
    public static let zoneCenter = CallFacePoint(0, 0, -0.35)
    public static let eyeUvHalf = (0.62, 0.36)
    public static let lookGain = 0.55

    public static func vertexRegion(_ v: Int, extraStart: Int = .max) -> Double {
        if v >= extraStart { return 1 }
        if v >= 68 { return 0 }
        return v <= 26 ? 0.25 : 1
    }

    static func signedArea(_ points: [CallFacePoint], _ v: [Int]) -> Double {
        let p = points[v[0]], q = points[v[1]], r = points[v[2]]
        return (q.x - p.x) * (r.y - p.y) - (r.x - p.x) * (q.y - p.y)
    }

    public static func warpVertices(mesh: CallFaceHeadMesh, layout: [CallFacePoint], live: [CallFacePoint], photoSize: Double, depths: [Double]? = nil) -> [Float] {
        var out = [Float](repeating: 0, count: mesh.triangles.count * 3 * vertexFloats)
        var at = 0
        for triangle in mesh.triangles {
            let folded = triangle.cull && signedArea(layout, triangle.v) * signedArea(live, triangle.v) < 0
            for (k, v) in triangle.v.enumerated() {
                let placed = live[folded ? triangle.v[0] : v]
                out[at] = Float(placed.x)
                out[at + 1] = Float(placed.y)
                out[at + 2] = Float(layout[v].x / photoSize)
                out[at + 3] = Float(layout[v].y / photoSize)
                out[at + 4] = Float(triangle.hole[k])
                out[at + 5] = Float(vertexRegion(v, extraStart: mesh.extraStart))
                out[at + 6] = Float(depths?[v] ?? 0)
                at += vertexFloats
            }
        }
        return out
    }

    public static func coverRect(imageWidth: Double, imageHeight: Double, width: Double, height: Double) -> [Double] {
        let image = imageWidth / CallFaceJS.max(1, imageHeight)
        let frame = width / CallFaceJS.max(1, height)
        if image > frame {
            let span = frame / image
            return [(1 - span) / 2, 0, span, 1]
        }
        let span = image / frame
        return [0, (1 - span) / 2, 1, span]
    }

    public static func hairRegion(_ pose: CallFacePose, width: Double, height: Double) -> CallFaceHairRegion {
        let center = CallFaceGeometry.projectPose(pose, hairCenter)
        let side = hairSide * pose.scale
        let size = CallFacePoint(CallFaceJS.min(side, width), CallFaceJS.min(side, height))
        func fit(_ value: Double, _ extent: Double, _ limit: Double) -> Double {
            CallFaceJS.min(CallFaceJS.max(value, extent / 2), limit - extent / 2)
        }
        return CallFaceHairRegion(center: CallFacePoint(fit(center.x, size.x, width), fit(center.y, size.y, height)), size: size)
    }

    public static func photoEyes(_ layout: [CallFacePoint], photoSize: Double) -> [[Double]] {
        [(36, 39, [36, 37, 38, 39, 40, 41]), (42, 45, [42, 43, 44, 45, 46, 47])].map { a, b, ring in
            let width = CallFaceJS.hypot(layout[b].x - layout[a].x, layout[b].y - layout[a].y) / photoSize
            let center = CallFaceGeometry.centroid(layout, ring)
            return [center.x / photoSize, center.y / photoSize, eyeUvHalf.0 * width, eyeUvHalf.1 * width]
        }
    }

    public static func lookUniform(_ look: [CallFacePoint?]?) -> [Double] {
        guard let look else { return [0, 0, 0, 0] }
        func pick(_ side: Int, _ k: Int) -> Double {
            guard side < look.count, let point = look[side] else { return 0 }
            let value = k == 0 ? point.x : point.y
            return value.isFinite ? value * lookGain : 0
        }
        return [pick(0, 0), pick(0, 1), pick(1, 0), pick(1, 1)]
    }

    public static func axisFrame(_ box: CallFaceBox) -> CallFaceAxisFrame {
        CallFaceAxisFrame(
            center: box.center,
            x: CallFacePoint(box.ux.x / box.half.x, box.ux.y / box.half.x),
            y: CallFacePoint(box.uy.x / box.half.y, box.uy.y / box.half.y)
        )
    }

    public static func zoneFrame(_ head: [CallFacePoint], _ pose: CallFacePose) -> CallFaceAxisFrame {
        let center = CallFaceGeometry.projectPose(pose, zoneCenter)
        let frame = CallFaceGeometry.faceFrame(head)
        let scale = CallFaceJS.or(pose.scale, frame.span)
        let ux = CallFacePoint(cos(pose.roll), sin(pose.roll))
        let uy = CallFacePoint(-ux.y, ux.x)
        return CallFaceAxisFrame(center: center, x: CallFacePoint(ux.x / scale, ux.y / scale), y: CallFacePoint(uy.x / scale, uy.y / scale))
    }
}
