import Foundation

public struct CallFaceBox: Equatable, Sendable {
    public let center: CallFacePoint
    public let ux: CallFacePoint
    public let uy: CallFacePoint
    public let half: CallFacePoint
}

public struct CallFaceImage: Equatable, Sendable {
    public let width: Int
    public let height: Int
    public let rgba: [UInt8]

    public init(width: Int, height: Int, rgba: [UInt8]) {
        self.width = width
        self.height = height
        self.rgba = rgba
    }
}

public struct CallFaceSkin: Equatable, Sendable {
    public let luma: Double
    public let red: Double
}

public struct CallFaceEyeReading: Equatable, Sendable {
    public let dark: Double
    public let edge: Double
    public let look: CallFacePoint
}

public struct CallFaceProbeCrop: Equatable, Sendable {
    public let minX: Double
    public let minY: Double
    public let scale: Double
    public let width: Int
    public let height: Int

    public func local(_ point: CallFacePoint) -> CallFacePoint {
        CallFacePoint((point.x - minX) * scale, (point.y - minY) * scale)
    }
}

public struct CallFaceExpressionReading: Equatable, Sendable {
    public let close: [Double]
    public let look: [CallFacePoint]
    public let tongue: Double
    public let skinRed: Double
    public let skinLuma: Double
}

public struct CallFaceBlinkInput: Sendable {
    public var edges: [Double]
    public var openness: [Double]?
    public var elapsed: Double
    public var time: Double
    public var moving: Bool

    public init(edges: [Double], openness: [Double]? = nil, elapsed: Double, time: Double, moving: Bool = false) {
        self.edges = edges
        self.openness = openness
        self.elapsed = elapsed
        self.time = time
        self.moving = moving
    }
}

public enum CallFaceExpression {
    static let probeSpan = 120.0
    static let eyeHalf = (0.62, 0.3)
    static let eyeGrid = (18, 10)
    static let darkRatio = 0.62
    static let stillSpansPerSecond = 2.2
    static let lookLimit = 0.32
    static let restSeconds = 3.0
    static let tongueGrid = (14, 12)
    static let tongueGate = (0.1, 0.24)
    static let tongueBaseRiseSeconds = 10.0
    static let upperLids = [(37, 41), (38, 40), (43, 47), (44, 46)]
    static let lowerLift = 0.15
    public static let eyeCorners = [(36, 39), (42, 45)]

    public static func lumaOf(_ r: Double, _ g: Double, _ b: Double) -> Double {
        0.299 * r + 0.587 * g + 0.114 * b
    }

    public static func rednessOf(_ r: Double, _ g: Double, _ b: Double) -> Double {
        (r - g) / (r + g + b + 12)
    }

    public static func eyeBox(_ points: [CallFacePoint], _ side: Int) -> CallFaceBox {
        let (a, b) = eyeCorners[side]
        let p = points[a]
        let q = points[b]
        let dx = q.x - p.x
        let dy = q.y - p.y
        let width = CallFaceJS.or(CallFaceJS.hypot(dx, dy), 1)
        let ux = CallFacePoint(dx / width, dy / width)
        let uy = CallFacePoint(-ux.y, ux.x)
        let lids = CallFaceGeometry.centroid(points, side == 1 ? [43, 44, 46, 47] : [37, 38, 40, 41])
        let mid = CallFacePoint((p.x + q.x) / 2, (p.y + q.y) / 2)
        let lift = (lids.x - mid.x) * uy.x + (lids.y - mid.y) * uy.y
        return CallFaceBox(
            center: CallFacePoint(mid.x + uy.x * lift * 0.5, mid.y + uy.y * lift * 0.5),
            ux: ux,
            uy: uy,
            half: CallFacePoint(eyeHalf.0 * width, eyeHalf.1 * width)
        )
    }

    public static func tongueWindow(_ points: [CallFacePoint]) -> CallFaceBox {
        let frame = CallFaceGeometry.faceFrame(points)
        let lip = CallFacePoint((points[66].x + points[57].x) / 2, (points[66].y + points[57].y) / 2)
        let chin = points[8]
        let down = CallFacePoint(chin.x - lip.x, chin.y - lip.y)
        let reach = CallFaceJS.or(CallFaceJS.hypot(down.x, down.y), frame.span * 0.6)
        let mouth = CallFaceJS.or(CallFaceJS.hypot(points[54].x - points[48].x, points[54].y - points[48].y), frame.span * 0.8)
        let uy = CallFacePoint(down.x / reach, down.y / reach)
        return CallFaceBox(
            center: CallFacePoint(lip.x + down.x * 0.38, lip.y + down.y * 0.38),
            ux: CallFacePoint(-uy.y, uy.x),
            uy: uy,
            half: CallFacePoint(mouth * 0.42, reach * 0.55)
        )
    }

    public static func sampleBox(_ image: CallFaceImage, _ box: CallFaceBox, _ grid: (Int, Int), _ visit: (Double, Double, Double, Double, Double) -> Void) {
        for j in 0..<grid.1 {
            let v = ((Double(j) + 0.5) / Double(grid.1)) * 2 - 1
            for i in 0..<grid.0 {
                let u = ((Double(i) + 0.5) / Double(grid.0)) * 2 - 1
                let x = CallFaceJS.round(box.center.x + box.ux.x * u * box.half.x + box.uy.x * v * box.half.y)
                let y = CallFaceJS.round(box.center.y + box.ux.y * u * box.half.x + box.uy.y * v * box.half.y)
                if !(x >= 0 && y >= 0 && x < Double(image.width) && y < Double(image.height)) { continue }
                let at = (Int(y) * image.width + Int(x)) * 4
                visit(Double(image.rgba[at]), Double(image.rgba[at + 1]), Double(image.rgba[at + 2]), u, v)
            }
        }
    }

    public static func skinReference(_ image: CallFaceImage, _ points: [CallFacePoint]) -> CallFaceSkin? {
        let frame = CallFaceGeometry.faceFrame(points)
        var luma = 0.0
        var red = 0.0
        var count = 0
        for (a, b) in [(2, 31), (14, 35), (3, 48), (13, 54)] {
            let center = CallFacePoint((points[a].x + points[b].x) / 2, (points[a].y + points[b].y) / 2)
            let box = CallFaceBox(center: center, ux: frame.ux, uy: frame.uy, half: CallFacePoint(frame.span * 0.1, frame.span * 0.1))
            sampleBox(image, box, (5, 5)) { r, g, bl, _, _ in
                luma += lumaOf(r, g, bl)
                red += rednessOf(r, g, bl)
                count += 1
            }
        }
        return count > 0 ? CallFaceSkin(luma: luma / Double(count), red: red / Double(count)) : nil
    }

    public static func measureEye(_ image: CallFaceImage, _ box: CallFaceBox, _ skin: CallFaceSkin) -> CallFaceEyeReading? {
        let threshold = skin.luma * darkRatio
        var grid = [Float](repeating: -1, count: eyeGrid.0 * eyeGrid.1)
        var total = 0
        var dark = 0
        var wx = 0.0
        var wy = 0.0
        var weight = 0.0
        sampleBox(image, box, eyeGrid) { r, g, b, u, v in
            total += 1
            let luma = lumaOf(r, g, b)
            let row = CallFaceJS.round(((v + 1) / 2) * Double(eyeGrid.1) - 0.5)
            let column = CallFaceJS.round(((u + 1) / 2) * Double(eyeGrid.0) - 0.5)
            grid[Int(row) * eyeGrid.0 + Int(column)] = Float(luma)
            if luma >= threshold { return }
            dark += 1
            let w = (threshold - luma) * (threshold - luma)
            wx += w * u
            wy += w * v
            weight += w
        }
        if total == 0 { return nil }
        var edge = 0.0
        var edges = 0
        for j in 1..<(eyeGrid.1 - 1) {
            for i in 1..<(eyeGrid.0 - 1) {
                let left = Double(grid[j * eyeGrid.0 + i - 1])
                let right = Double(grid[j * eyeGrid.0 + i + 1])
                if left < 0 || right < 0 { continue }
                edge += abs(right - left)
                edges += 1
            }
        }
        return CallFaceEyeReading(
            dark: Double(dark) / Double(total),
            edge: edges > 0 ? edge / Double(edges) / CallFaceJS.max(skin.luma, 1) : 0,
            look: weight > 0 ? CallFacePoint(wx / weight, wy / weight) : CallFacePoint(0, 0)
        )
    }

    public static func measureTongue(_ image: CallFaceImage, _ box: CallFaceBox, _ skin: CallFaceSkin) -> Double {
        var total = 0
        var red = 0.0
        sampleBox(image, box, tongueGrid) { r, g, b, _, _ in
            total += 1
            let luma = lumaOf(r, g, b)
            if luma < skin.luma * 0.22 || luma > CallFaceJS.max(skin.luma * 2.4, 60) { return }
            red += CallFaceGeometry.clampValue((rednessOf(r, g, b) - skin.red - 0.05) / 0.08, 0, 1)
        }
        return total > 0 ? red / Double(total) : 0
    }

    public static func eyeOpenness(_ points: [CallFacePoint], _ side: Int) -> Double {
        let (a, b) = eyeCorners[side]
        let width = CallFaceJS.or(CallFaceJS.hypot(points[b].x - points[a].x, points[b].y - points[a].y), 1)
        let pairs = side == 1 ? [(43, 47), (44, 46)] : [(37, 41), (38, 40)]
        let gap = pairs.reduce(0.0) { sum, pair in sum + CallFaceJS.hypot(points[pair.0].x - points[pair.1].x, points[pair.0].y - points[pair.1].y) }
        return gap / Double(pairs.count) / width
    }

    public static func pairClosure(_ close: [Double]) -> [Double] {
        let low = CallFaceJS.min(close[0], close[1])
        return close.map { low + ($0 - low) * (1 - 0.75) }
    }

    public static func closeLids(_ points: [CallFacePoint], _ close: [Double]?) -> [CallFacePoint] {
        guard let close, close[0] > 0.01 || close[1] > 0.01 else { return points }
        var out = points
        for (k, (upper, lower)) in upperLids.enumerated() {
            let amount = CallFaceGeometry.clampValue(close[k < 2 ? 0 : 1], 0, 1)
            if amount <= 0.01 { continue }
            let meet = CallFacePoint(
                out[lower].x + (out[upper].x - out[lower].x) * lowerLift,
                out[lower].y + (out[upper].y - out[lower].y) * lowerLift
            )
            out[upper] = CallFacePoint(out[upper].x + (meet.x - out[upper].x) * amount, out[upper].y + (meet.y - out[upper].y) * amount, out[upper].z)
            out[lower] = CallFacePoint(out[lower].x + (meet.x - out[lower].x) * amount, out[lower].y + (meet.y - out[lower].y) * amount, out[lower].z)
        }
        return out
    }

    public static func probeCrop(_ points: [CallFacePoint]) -> CallFaceProbeCrop {
        let frame = CallFaceGeometry.faceFrame(points)
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
        let pad = frame.span * 0.3
        minX = (minX - pad).rounded(.down)
        minY = (minY - pad).rounded(.down)
        maxX = (maxX + pad).rounded(.up)
        maxY = (maxY + pad).rounded(.up)
        let scale = CallFaceJS.min(1, probeSpan / frame.span)
        let width = Int(CallFaceJS.max(4, CallFaceJS.round((maxX - minX) * scale)))
        let height = Int(CallFaceJS.max(4, CallFaceJS.round((maxY - minY) * scale)))
        return CallFaceProbeCrop(minX: minX, minY: minY, scale: scale, width: width, height: height)
    }
}

public final class CallFaceBlinkTracker {
    static let openMin = 0.05
    static let closeStart = 0.52
    static let closeFull = 0.28
    static let baseRiseSeconds = 0.8
    static let baseFallSeconds = 6.0
    static let lidOpen = 0.9
    static let lidShut = 0.6
    static let lidBaseRiseSeconds = 1.5
    static let lidBaseFallSeconds = 8.0
    static let lidMin = 0.12
    static let enter = 0.4
    static let leave = 0.18

    final class Eye {
        var base = 0.0
        var lidBase = 0.0
        var closing = false
        let close = CallFaceOneEuroFilter(minCutoff: 4, beta: 2)
    }

    private let eyes = [Eye(), Eye()]

    public init() {}

    public func reset() {
        for eye in eyes {
            eye.base = 0
            eye.lidBase = 0
            eye.closing = false
            eye.close.reset()
        }
    }

    public func update(_ input: CallFaceBlinkInput) -> [Double] {
        typealias T = CallFaceBlinkTracker
        let clamp = CallFaceGeometry.clampValue
        var raw = [0.0, 0.0]
        for side in 0..<2 {
            let eye = eyes[side]
            let edge = input.edges[side]
            if !edge.isFinite { continue }
            var gate = 1.0
            if let openness = input.openness, openness[side].isFinite {
                let open = openness[side]
                if !CallFaceJS.truthy(eye.lidBase) {
                    eye.lidBase = open
                } else if open >= eye.lidBase {
                    eye.lidBase += (open - eye.lidBase) * clamp(input.elapsed / T.lidBaseRiseSeconds, 0, 1)
                } else if !eye.closing {
                    eye.lidBase -= (eye.lidBase - open) * clamp(input.elapsed / T.lidBaseFallSeconds, 0, 1)
                }
                if eye.lidBase >= T.lidMin { gate = clamp((T.lidOpen - open / eye.lidBase) / (T.lidOpen - T.lidShut), 0, 1) }
            }
            if !CallFaceJS.truthy(eye.base) {
                eye.base = edge
            } else if edge >= eye.base {
                eye.base += (edge - eye.base) * clamp(input.elapsed / T.baseRiseSeconds, 0, 1)
            } else if !eye.closing {
                eye.base -= (eye.base - edge) * clamp(input.elapsed / T.baseFallSeconds, 0, 1)
            }
            var amount = 0.0
            if eye.base >= T.openMin { amount = clamp((T.closeStart - edge / eye.base) / (T.closeStart - T.closeFull), 0, 1) }
            raw[side] = input.moving ? 0 : amount * gate
        }
        let paired = CallFaceExpression.pairClosure(raw)
        return paired.enumerated().map { side, value in
            let eye = eyes[side]
            if !input.edges[side].isFinite { return 0 }
            var amount = value
            if eye.closing ? amount < T.leave : amount < T.enter {
                eye.closing = false
                amount = 0
            } else {
                eye.closing = true
            }
            return clamp(eye.close.filter(amount, time: input.time), 0, 1)
        }
    }
}

public final class CallFaceExpressionProbe {
    final class Eye {
        let lookX = CallFaceOneEuroFilter(minCutoff: 1.2, beta: 0.8)
        let lookY = CallFaceOneEuroFilter(minCutoff: 1.2, beta: 0.8)
        var look = CallFacePoint(0, 0)
        var rest: CallFacePoint?
    }

    private let blinks = CallFaceBlinkTracker()
    private let eyes = [Eye(), Eye()]
    private var tongueBase = -1.0
    private let tongueGate = CallFaceOneEuroFilter(minCutoff: 3, beta: 1)
    private var last = 0.0
    private var lastMid: CallFacePoint?

    public init() {}

    public func reset() {
        lastMid = nil
        blinks.reset()
        for eye in eyes {
            eye.lookX.reset()
            eye.lookY.reset()
            eye.look = CallFacePoint(0, 0)
            eye.rest = nil
        }
        tongueBase = -1
        tongueGate.reset()
        last = 0
    }

    public func measure(image: CallFaceImage, crop: CallFaceProbeCrop, points: [CallFacePoint], time: Double, pupils: [CallFacePoint]? = nil, lids: Bool = false) -> CallFaceExpressionReading? {
        typealias E = CallFaceExpression
        let clamp = CallFaceGeometry.clampValue
        let local = points.map(crop.local)
        guard let skin = E.skinReference(image, local), skin.luma >= 18 else { return nil }
        let elapsed = CallFaceJS.truthy(last) ? clamp((time - last) / 1000, 0, 1) : 0
        last = time
        let frame = CallFaceGeometry.faceFrame(points)
        var speed = 0.0
        if let lastMid, elapsed > 0 {
            speed = CallFaceJS.hypot(frame.mid.x - lastMid.x, frame.mid.y - lastMid.y) / frame.span / elapsed
        }
        lastMid = frame.mid
        let boxes = [E.eyeBox(local, 0), E.eyeBox(local, 1)]
        let found = boxes.map { E.measureEye(image, $0, skin) }
        let close = blinks.update(CallFaceBlinkInput(
            edges: found.map { $0?.edge ?? .nan },
            openness: lids ? [E.eyeOpenness(points, 0), E.eyeOpenness(points, 1)] : nil,
            elapsed: elapsed,
            time: time,
            moving: speed > E.stillSpansPerSecond
        ))
        var look = [CallFacePoint(0, 0), CallFacePoint(0, 0)]
        for side in 0..<2 {
            let eye = eyes[side]
            guard let seen = found[side] else { continue }
            let box = boxes[side]
            var gazeAt = seen.look
            if let pupils, side < pupils.count {
                let pupil = pupils[side]
                let px = (pupil.x - crop.minX) * crop.scale - box.center.x
                let py = (pupil.y - crop.minY) * crop.scale - box.center.y
                gazeAt = CallFacePoint((px * box.ux.x + py * box.ux.y) / box.half.x, (px * box.uy.x + py * box.uy.y) / box.half.y)
            }
            if close[side] < 0.4 {
                let rest = eye.rest ?? CallFacePoint(gazeAt.x, gazeAt.y)
                let pull = clamp(elapsed / E.restSeconds, 0, 1)
                let settled = CallFacePoint(rest.x + (gazeAt.x - rest.x) * pull, rest.y + (gazeAt.y - rest.y) * pull)
                eye.rest = settled
                eye.look = CallFacePoint(
                    clamp(eye.lookX.filter(gazeAt.x - settled.x, time: time), -E.lookLimit, E.lookLimit),
                    clamp(eye.lookY.filter(gazeAt.y - settled.y, time: time), -E.lookLimit, E.lookLimit)
                )
            }
            look[side] = eye.look
        }
        let mouthBox = E.tongueWindow(local)
        let red = E.measureTongue(image, mouthBox, skin)
        if tongueBase < 0 {
            tongueBase = red
        } else if red < tongueBase {
            tongueBase += (red - tongueBase) * 0.5
        } else {
            tongueBase += (red - tongueBase) * (elapsed / E.tongueBaseRiseSeconds)
        }
        let raised = red - tongueBase
        let gate = clamp((raised - E.tongueGate.0) / (E.tongueGate.1 - E.tongueGate.0), 0, 1)
        return CallFaceExpressionReading(
            close: close,
            look: look,
            tongue: clamp(tongueGate.filter(gate, time: time), 0, 1),
            skinRed: skin.red,
            skinLuma: skin.luma / 255
        )
    }
}
