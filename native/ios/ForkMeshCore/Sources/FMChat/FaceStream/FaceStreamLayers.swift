import Foundation

public struct FaceStreamLayers: Equatable, Sendable {
    public let head: [Float]
    public let removal: [Float]
}

public struct FaceStreamMouthShape: Equatable, Sendable {
    public let center: CallFacePoint
    public let axis: CallFacePoint
    public let halfWidth: Double
    public let halfGap: Double
    public let span: Double

    public init(center: CallFacePoint, axis: CallFacePoint, halfWidth: Double, halfGap: Double, span: Double) {
        self.center = center
        self.axis = axis
        self.halfWidth = halfWidth
        self.halfGap = halfGap
        self.span = span
    }
}

public struct FaceStreamMouthColours: Equatable, Sendable {
    public let lip: [Double]
    public let teeth: [Double]
    public let skin: [Double]
}

public struct FaceStreamMouthBlend: Equatable, Sendable {
    public let a: Int
    public let b: Int
    public let mix: Double
    public let opacity: Double
}

public struct FaceStreamPlace: Equatable, Sendable {
    public let points: [CallFacePoint]
    public let pose: FaceStreamPose
    public let matrix: [Double]
    public let pad: Int
    public let width: Int
    public let height: Int

    public init(points: [CallFacePoint], pose: FaceStreamPose, matrix: [Double], pad: Int, width: Int, height: Int) {
        self.points = points
        self.pose = pose
        self.matrix = matrix
        self.pad = pad
        self.width = width
        self.height = height
    }
}

public struct FaceStreamAnchor: Equatable, Sendable {
    public let offsets: [CallFacePoint]
    public let roll: Double
    public let span: Double
    public let width: Int
    public let height: Int
}

public struct FaceStreamFace: Equatable, Sendable {
    public let head: CallFaceHead
    public let aux: [UInt8]
    public let placed: [CallFacePoint]
}

extension FaceStreamView {
    public static let photoSize = 512
    public static let photoSpan = 96.0
    public static let photoMid = CallFacePoint(256, 210)
    public static let pad = 0.2
    public static let mouthVertexFloats = 7
    static let outerRing = Array(48..<60)
    static let innerRing = Array(60..<68)
    static let mouthBand: [(Int, Int)] = [(48, 60), (49, 61), (50, 61), (51, 62), (52, 63), (53, 63), (54, 64), (55, 65), (56, 65), (57, 66), (58, 67), (59, 67)]

    public static func boxMean(_ values: [Float], _ w: Int, _ h: Int, _ radius: Int) -> [Float] {
        var tmp = [Float](repeating: 0, count: w * h)
        var out = [Float](repeating: 0, count: w * h)
        for y in 0..<h {
            for x in 0..<w {
                var sum: Float = 0
                let lo = max(0, x - radius), hi = min(w - 1, x + radius)
                for k in lo...hi { sum += values[y * w + k] }
                tmp[y * w + x] = sum / Float(hi - lo + 1)
            }
        }
        for y in 0..<h {
            let lo = max(0, y - radius), hi = min(h - 1, y + radius)
            for x in 0..<w {
                var sum: Float = 0
                for k in lo...hi { sum += tmp[k * w + x] }
                out[y * w + x] = sum / Float(hi - lo + 1)
            }
        }
        return out
    }

    static func maxFilter(_ values: [Float], _ w: Int, _ h: Int, _ radius: Int) -> [Float] {
        var tmp = [Float](repeating: 0, count: w * h)
        var out = [Float](repeating: 0, count: w * h)
        for y in 0..<h {
            for x in 0..<w {
                var m: Float = 0
                for k in max(0, x - radius)...min(w - 1, x + radius) { m = max(m, values[y * w + k]) }
                tmp[y * w + x] = m
            }
        }
        for y in 0..<h {
            for x in 0..<w {
                var m: Float = 0
                for k in max(0, y - radius)...min(h - 1, y + radius) { m = max(m, tmp[k * w + x]) }
                out[y * w + x] = m
            }
        }
        return out
    }

    public static func coarseMask(_ reference: FaceStreamReference) -> [Float] {
        let w = reference.width, h = reference.height, mw = reference.maskW, mh = reference.maskH
        var out = [Float](repeating: 0, count: w * h)
        guard let mask = reference.mask, mw > 0, mh > 0 else { return out }
        func at(_ x: Int, _ y: Int) -> Float { Float(mask[min(max(y, 0), mh - 1) * mw + min(max(x, 0), mw - 1)]) }
        for y in 0..<h {
            let fy = (Float(y) + 0.5) / Float(h) * Float(mh) - 0.5
            let y0 = Int(fy.rounded(.down))
            let ty = fy - Float(y0)
            for x in 0..<w {
                let fx = (Float(x) + 0.5) / Float(w) * Float(mw) - 0.5
                let x0 = Int(fx.rounded(.down))
                let tx = fx - Float(x0)
                let top = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx
                let bottom = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx
                out[y * w + x] = top * (1 - ty) + bottom * ty
            }
        }
        return out
    }

    public static func matte(_ reference: FaceStreamReference, rgba: [UInt8]) -> [Float]? {
        guard reference.mask != nil, reference.maskW > 0 else { return nil }
        let w = reference.width, h = reference.height, n = w * h
        let coarse = coarseMask(reference)
        var guide = [Float](repeating: 0, count: n)
        for i in 0..<n { guide[i] = (0.299 * Float(rgba[i * 4]) + 0.587 * Float(rgba[i * 4 + 1]) + 0.114 * Float(rgba[i * 4 + 2])) / 255 }
        let radius = max(2, Int((Double(w) / 64).rounded()))
        let eps: Float = 0.002
        let ii = (0..<n).map { guide[$0] * guide[$0] }
        let ip = (0..<n).map { guide[$0] * coarse[$0] }
        let meanI = boxMean(guide, w, h, radius)
        let meanP = boxMean(coarse, w, h, radius)
        let meanII = boxMean(ii, w, h, radius)
        let meanIP = boxMean(ip, w, h, radius)
        var a = [Float](repeating: 0, count: n)
        var b = [Float](repeating: 0, count: n)
        for i in 0..<n {
            let variance = meanII[i] - meanI[i] * meanI[i]
            a[i] = (meanIP[i] - meanI[i] * meanP[i]) / (variance + eps)
            b[i] = meanP[i] - a[i] * meanI[i]
        }
        let meanA = boxMean(a, w, h, radius)
        let meanB = boxMean(b, w, h, radius)
        let band = boxMean(coarse, w, h, max(1, Int((Double(w) / Double(reference.maskW)).rounded())))
        return (0..<n).map { i in
            let q = min(max(meanA[i] * guide[i] + meanB[i], 0), 1)
            return max(q * Float(smoothstep(0, 0.3, Double(band[i]))), Float(smoothstep(0.7, 1, Double(band[i]))))
        }
    }

    public static func layers(_ reference: FaceStreamReference, alpha: [Float]) -> FaceStreamLayers {
        let w = reference.width, h = reference.height
        let frame = CallFaceGeometry.faceFrame(reference.points)
        let chin = ((reference.points[8].x - frame.mid.x) * frame.uy.x + (reference.points[8].y - frame.mid.y) * frame.uy.y) / frame.span
        var head = [Float](repeating: 0, count: w * h)
        var removal = [Float](repeating: 0, count: w * h)
        for y in 0..<h {
            for x in 0..<w {
                let i = y * w + x
                let qy = ((Double(x) + 0.5 - frame.mid.x) * frame.uy.x + (Double(y) + 0.5 - frame.mid.y) * frame.uy.y) / frame.span
                head[i] = alpha[i] * Float(1 - smoothstep(chin + 0.25, chin + 0.9, qy))
                removal[i] = alpha[i] * Float(1 - smoothstep(chin + 0.15, chin + 0.6, qy))
            }
        }
        return FaceStreamLayers(head: head, removal: removal)
    }

    public static func grow(_ values: [Float], _ w: Int, _ h: Int) -> [Float] {
        let radius = max(1, Int((Double(w) / 128).rounded()))
        let grown = maxFilter(values, w, h, radius)
        let soft = boxMean(grown, w, h, radius)
        return (0..<(w * h)).map { min(1, max(grown[$0], soft[$0] * 1.5)) }
    }

    public static func plate(rgba: [UInt8], width w: Int, height h: Int, removal: [Float]) -> [UInt8] {
        let radius = max(1, Int((Double(w) / 100).rounded()))
        let soft = planes(pushPull(rgba: rgba, width: w, height: h, removal: removal), w, h).map { blur(blur($0, w, h, radius), w, h, radius) }
        var out = [UInt8](repeating: 255, count: w * h * 4)
        for i in 0..<(w * h) {
            let r = min(max(removal[i], 0), 1)
            for c in 0..<3 { out[i * 4 + c] = UInt8(min(max((Float(rgba[i * 4 + c]) * (1 - r) + soft[c][i] * 255 * r).rounded(), 0), 255)) }
        }
        return out
    }

    public static func unmix(rgba: [UInt8], plate: [UInt8], alpha: [Float]) -> [UInt8] {
        var out = [UInt8](repeating: 255, count: rgba.count)
        for i in 0..<alpha.count {
            let a = alpha[i]
            for c in 0..<3 {
                let at = i * 4 + c
                if a > 0.98 || a < 0.01 {
                    out[at] = rgba[at]
                } else {
                    let v = (Float(rgba[at]) - (1 - a) * Float(plate[at])) / max(a, 0.05)
                    out[at] = UInt8(min(max(v.rounded(), 0), 255))
                }
            }
        }
        return out
    }

    public static func padFloats(_ values: [Float], _ w: Int, _ h: Int, _ pad: Int) -> [Float] {
        let pw = w + 2 * pad, ph = h + 2 * pad
        var out = [Float](repeating: 0, count: pw * ph)
        for y in 0..<ph {
            let sy = min(max(y - pad, 0), h - 1)
            for x in 0..<pw { out[y * pw + x] = values[sy * w + min(max(x - pad, 0), w - 1)] }
        }
        return out
    }

    public static func padPixels(_ rgba: [UInt8], _ w: Int, _ h: Int, _ pad: Int) -> [UInt8] {
        let pw = w + 2 * pad, ph = h + 2 * pad
        var out = [UInt8](repeating: 255, count: pw * ph * 4)
        for y in 0..<ph {
            let sy = min(max(y - pad, 0), h - 1)
            for x in 0..<pw {
                let from = (sy * w + min(max(x - pad, 0), w - 1)) * 4
                let to = (y * pw + x) * 4
                for c in 0..<4 { out[to + c] = rgba[from + c] }
            }
        }
        return out
    }

    public static func photoTransform(_ points: [CallFacePoint]) -> [Double] {
        let frame = CallFaceGeometry.faceFrame(points)
        let scale = photoSpan / frame.span
        let c = cos(-frame.roll) * scale
        let s = sin(-frame.roll) * scale
        return [c, s, -s, c, photoMid.x - (c * frame.mid.x - s * frame.mid.y), photoMid.y - (s * frame.mid.x + c * frame.mid.y)]
    }

    public static func streamHead(placed: [CallFacePoint], ring: [CallFacePoint]) -> CallFaceHead {
        let model = CallFaceGeometry.headModel(placed, ring)
        return CallFaceHead(layout: CallFaceGeometry.headLayout(placed, ring), model: model, mesh: CallFaceGeometry.buildHeadMesh(model, CallFaces.template))
    }

    public static func face(photo: [UInt8], person: [Float], head: [Float], points: [CallFacePoint], matrix: [Double]) -> FaceStreamFace {
        let size = photoSize
        let placed = points.map { CallFaceCustom.apply(matrix, $0) }
        let polygons = CallFaceCustom.polygons(placed)
        let parts = CallFaceCustom.buildMatte(
            pixels: photo, size: size, points: placed, person: person,
            faceMask: CallFaceCustom.rasterize(polygons.face, size: size),
            innerMask: CallFaceCustom.rasterize(polygons.inner, size: size),
            neckMask: CallFaceCustom.rasterize(polygons.neck, size: size))
        var aux = [UInt8](repeating: 255, count: size * size * 4)
        func byte(_ v: Float) -> UInt8 { UInt8(min(max((v * 255).rounded(), 0), 255)) }
        for i in 0..<(size * size) {
            aux[i * 4] = byte(head[i])
            aux[i * 4 + 1] = byte(parts.skin[i])
            aux[i * 4 + 2] = byte(parts.hair[i])
        }
        let frame = CallFaceGeometry.faceFrame(placed)
        let ring = CallFaceGeometry.pointsFromFlat(CallFaceCustom.headRing(head, size: size, mid: frame.mid, span: frame.span))
        let rounded = placed.map { CallFacePoint(CallFaceJS.round($0.x * 10) / 10, CallFaceJS.round($0.y * 10) / 10) }
        return FaceStreamFace(head: streamHead(placed: rounded, ring: ring), aux: aux, placed: rounded)
    }

    public static func mouthShape(_ points: [CallFacePoint]) -> FaceStreamMouthShape {
        let frame = CallFaceGeometry.faceFrame(points)
        var cx = 0.0, cy = 0.0
        for i in 60..<68 {
            cx += points[i].x / 8
            cy += points[i].y / 8
        }
        let dx = points[64].x - points[60].x
        let dy = points[64].y - points[60].y
        let length = (dx * dx + dy * dy).squareRoot()
        let width = length > 0 ? length : frame.span * 0.8
        let axis = CallFacePoint(dx / width, dy / width)
        let uy = CallFacePoint(-axis.y, axis.x)
        var gap = 0.0
        for (up, down) in [(61, 67), (62, 66), (63, 65)] {
            gap = max(gap, (points[down].x - points[up].x) * uy.x + (points[down].y - points[up].y) * uy.y)
        }
        return FaceStreamMouthShape(center: CallFacePoint(cx, cy), axis: axis, halfWidth: max(width / 2, 1e-3), halfGap: max(gap / 2, 0), span: frame.span)
    }

    static func inside(_ ring: [CallFacePoint], _ x: Double, _ y: Double) -> Bool {
        var result = false
        var j = ring.count - 1
        for i in 0..<ring.count {
            let a = ring[i], b = ring[j]
            if (a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x { result.toggle() }
            j = i
        }
        return result
    }

    static func regionMean(_ rgba: [UInt8], _ w: Int, _ h: Int, bounds ring: [CallFacePoint], _ test: (Double, Double) -> Bool) -> (mean: [Double], values: [[Double]])? {
        let xs = ring.map(\.x), ys = ring.map(\.y)
        let x0 = min(max(Int((xs.min() ?? 0).rounded(.down)), 0), w - 1), x1 = min(max(Int((xs.max() ?? 0).rounded(.up)), 0), w - 1)
        let y0 = min(max(Int((ys.min() ?? 0).rounded(.down)), 0), h - 1), y1 = min(max(Int((ys.max() ?? 0).rounded(.up)), 0), h - 1)
        guard x0 <= x1, y0 <= y1 else { return nil }
        var sum = [0.0, 0.0, 0.0]
        var values: [[Double]] = []
        for y in y0...y1 {
            for x in x0...x1 where test(Double(x) + 0.5, Double(y) + 0.5) {
                let at = (y * w + x) * 4
                let v = [Double(rgba[at]) / 255, Double(rgba[at + 1]) / 255, Double(rgba[at + 2]) / 255]
                values.append(v)
                for c in 0..<3 { sum[c] += v[c] }
            }
        }
        guard !values.isEmpty else { return nil }
        return (sum.map { $0 / Double(values.count) }, values)
    }

    static func lipRing(_ points: [CallFacePoint], offset: Int) -> (outer: [CallFacePoint], inner: [CallFacePoint], feather: [CallFacePoint]) {
        let outer = outerRing.map { points[$0 - offset] }
        let inner = innerRing.map { points[$0 - offset] }
        let cx = outer.reduce(0) { $0 + $1.x } / 12, cy = outer.reduce(0) { $0 + $1.y } / 12
        return (outer, inner, outer.map { CallFacePoint(cx + ($0.x - cx) * 1.35, cy + ($0.y - cy) * 1.35) })
    }

    static func lipSkinMean(_ rgba: [UInt8], _ w: Int, _ h: Int, _ points: [CallFacePoint], offset: Int = 0) -> [Double]? {
        let ring = lipRing(points, offset: offset)
        return regionMean(rgba, w, h, bounds: ring.feather) { inside(ring.feather, $0, $1) && !inside(ring.outer, $0, $1) }?.mean
    }

    public static func mouthColours(rgba: [UInt8], width w: Int, height h: Int, points: [CallFacePoint]) -> FaceStreamMouthColours {
        let ring = lipRing(points, offset: 0)
        func luma(_ c: [Double]) -> Double { 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2] }
        let skin = lipSkinMean(rgba, w, h, points) ?? [0.62, 0.48, 0.4]
        let lip = regionMean(rgba, w, h, bounds: ring.outer) { inside(ring.outer, $0, $1) && !inside(ring.inner, $0, $1) }?.mean
            ?? [skin[0] * 0.85, skin[1] * 0.6, skin[2] * 0.62]
        var teeth: [Double]?
        if CallFaceGeometry.mouthGap(points) > 0.06, let inner = regionMean(rgba, w, h, bounds: ring.inner, { inside(ring.inner, $0, $1) }), inner.values.count >= 6 {
            let sorted = inner.values.sorted { luma($0) > luma($1) }
            let top = Array(sorted.prefix(max(2, sorted.count * 3 / 10)))
            let mean = (0..<3).map { c in top.reduce(0) { $0 + $1[c] } / Double(top.count) }
            if luma(mean) > luma(skin) * 0.9 { teeth = mean }
        }
        let level = min(max(luma(skin) / 0.55, 0.55), 1.15)
        return FaceStreamMouthColours(lip: lip, teeth: teeth ?? [0.86 * level, 0.83 * level, 0.76 * level].map { min(max($0, 0), 1) }, skin: skin)
    }

    public static func atlasGains(sheet: [UInt8], sheetWidth: Int, atlas: FaceStreamAtlas, rgba: [UInt8], width w: Int, height h: Int, points: [CallFacePoint]) -> [[Double]] {
        let reference = lipSkinMean(rgba, w, h, points)
        return atlas.tiles.enumerated().map { k, tile in
            guard let reference else { return [1, 1, 1] }
            var pixels = [UInt8](repeating: 0, count: atlas.tileW * atlas.tileH * 4)
            for y in 0..<atlas.tileH {
                for x in 0..<atlas.tileW {
                    for c in 0..<4 { pixels[(y * atlas.tileW + x) * 4 + c] = sheet[(y * sheetWidth + k * atlas.tileW + x) * 4 + c] }
                }
            }
            guard let mine = lipSkinMean(pixels, atlas.tileW, atlas.tileH, tile.points, offset: 48) else { return [1, 1, 1] }
            return (0..<3).map { min(max((reference[$0] + 0.01) / (mine[$0] + 0.01), 0.6), 1.6) }
        }
    }

    public static func mouthBlend(_ tiles: [FaceStreamAtlasTile], expression: FaceStreamExpression, points: [CallFacePoint]?) -> FaceStreamMouthBlend? {
        guard !tiles.isEmpty else { return nil }
        let m = expression.mouth, s = expression.smile
        let r = points.map { FaceStreamSending.lipRatio($0) }
        let floor = max(0.08, 0.5 * m)
        let ranked = tiles.enumerated().compactMap { i, tile -> (Int, Double)? in
            guard tile.mouth >= floor else { return nil }
            let shape = r.map { 0.5 * abs($0 - FaceStreamSending.lipRatio(tile.points, offset: 48)) } ?? 0
            return (i, abs(m - tile.mouth) + 0.5 * abs(s - tile.smile) + shape)
        }.sorted { $0.1 < $1.1 || ($0.1 == $1.1 && $0.0 < $1.0) }
        guard let a = ranked.first else { return nil }
        let b = ranked.count > 1 ? ranked[1] : a
        let mix = ranked.count < 2 || a.1 + b.1 < 1e-9 ? 0 : a.1 / (a.1 + b.1)
        return FaceStreamMouthBlend(a: a.0, b: b.0, mix: mix, opacity: smoothstep(0.08, 0.2, m) * (1 - smoothstep(0.3, 0.6, a.1)))
    }

    public static func mouthMesh(_ points: [CallFacePoint], tileA: FaceStreamAtlasTile, tileB: FaceStreamAtlasTile, tileW: Int, tileH: Int, lipAlpha: Double = 0.85) -> [Float] {
        let live = lipRing(Array(points[48..<68]), offset: 48)
        let a = lipRing(tileA.points, offset: 48)
        let b = lipRing(tileB.points, offset: 48)
        func centre(_ ring: [CallFacePoint]) -> CallFacePoint {
            CallFacePoint(ring.reduce(0) { $0 + $1.x } / 8, ring.reduce(0) { $0 + $1.y } / 8)
        }
        enum Key { case centre, point(Int), feather(Int) }
        func vertex(_ key: Key) -> (CallFacePoint, CallFacePoint, CallFacePoint, Double) {
            switch key {
            case .centre: return (centre(live.inner), centre(a.inner), centre(b.inner), 1)
            case .feather(let k): return (live.feather[k], a.feather[k], b.feather[k], 0)
            case .point(let i):
                func at(_ ring: (outer: [CallFacePoint], inner: [CallFacePoint], feather: [CallFacePoint])) -> CallFacePoint { i >= 60 ? ring.inner[i - 60] : ring.outer[i - 48] }
                return (at(live), at(a), at(b), i >= 60 ? 1 : lipAlpha)
            }
        }
        var triangles: [[Key]] = []
        for k in 0..<8 { triangles.append([.centre, .point(60 + k), .point(60 + (k + 1) % 8)]) }
        for j in 0..<12 {
            let (o1, i1) = mouthBand[j]
            let (o2, i2) = mouthBand[(j + 1) % 12]
            triangles.append([.point(o1), .point(o2), .point(i2)])
            if i1 != i2 { triangles.append([.point(o1), .point(i2), .point(i1)]) }
        }
        for j in 0..<12 {
            triangles.append([.point(48 + j), .point(48 + (j + 1) % 12), .feather((j + 1) % 12)])
            triangles.append([.point(48 + j), .feather((j + 1) % 12), .feather(j)])
        }
        var out: [Float] = []
        out.reserveCapacity(triangles.count * 3 * mouthVertexFloats)
        for triangle in triangles {
            for key in triangle {
                let (p, ua, ub, alpha) = vertex(key)
                out += [Float(p.x), Float(p.y), Float(ua.x / Double(tileW)), Float(ua.y / Double(tileH)), Float(ub.x / Double(tileW)), Float(ub.y / Double(tileH)), Float(alpha)]
            }
        }
        return out
    }

    public static func warpPoints(_ headPoints: [CallFacePoint], drawn: [CallFacePoint], mouthStart: Int, anchor: FaceStreamAnchor?) -> [CallFacePoint] {
        var out = headPoints.prefix(mouthStart).map { CallFacePoint($0.x, $0.y) }
        if let anchor {
            let live = CallFaceGeometry.faceFrame(drawn)
            let turn = live.roll - anchor.roll
            let grow = live.span / anchor.span
            let c = cos(turn) * grow, s = sin(turn) * grow
            for v in 0..<mouthStart {
                let d = anchor.offsets[v]
                out[v] = CallFacePoint(out[v].x + c * d.x - s * d.y, out[v].y + s * d.x + c * d.y)
            }
        }
        for i in 17..<68 { out[i] = CallFacePoint(drawn[i].x, drawn[i].y) }
        let frame = CallFaceGeometry.faceFrame(out)
        for (up, down) in [(61, 67), (62, 66), (63, 65)] {
            let gap = (out[down].x - out[up].x) * frame.uy.x + (out[down].y - out[up].y) * frame.uy.y
            if gap < 0 {
                let mid = CallFacePoint((out[up].x + out[down].x) / 2, (out[up].y + out[down].y) / 2)
                out[up] = mid
                out[down] = mid
            }
        }
        return out + CallFaceGeometry.mouthExtras(out)
    }

    public static func anchor(head: CallFaceHead, place: FaceStreamPlace, width: Int, height: Int) -> FaceStreamAnchor {
        let sx = Double(width) / Double(place.width)
        let sy = Double(height) / Double(place.height)
        let r3 = FaceStream.rotationRows(place.pose.rotation)[2]
        let depth = place.pose.scale * (sx + sy) * 0.5
        let live = place.points.enumerated().map { i, p in CallFacePoint(p.x * sx, p.y * sy, depth * FaceStream.dot3(r3, FaceStream.template[i])) }
        let fitted = CallFaceGeometry.liveHead(head.model, live)
        let m = place.matrix
        let det = m[0] * m[3] - m[1] * m[2]
        let inverse = det == 0 ? 1e-9 : det
        let mouthStart = head.mesh.mouthStart
        let offsets = (0..<mouthStart).map { v -> CallFacePoint in
            let l = head.layout[v]
            let px = (m[3] * (l.x - m[4]) - m[2] * (l.y - m[5])) / inverse - Double(place.pad)
            let py = (-m[1] * (l.x - m[4]) + m[0] * (l.y - m[5])) / inverse - Double(place.pad)
            return CallFacePoint(px * sx - fitted.points[v].x, py * sy - fitted.points[v].y)
        }
        let frame = CallFaceGeometry.faceFrame(live)
        return FaceStreamAnchor(offsets: offsets, roll: frame.roll, span: frame.span, width: width, height: height)
    }
}
