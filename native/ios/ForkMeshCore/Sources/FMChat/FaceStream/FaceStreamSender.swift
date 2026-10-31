import Foundation

public struct FaceStreamSolvedPose: Equatable, Sendable {
    public let pose: FaceStreamPose
    public let yaw: Double
    public let pitch: Double
    public let roll: Double
}

public struct FaceStreamLight: Equatable, Sendable {
    public let mean: Double
    public let contrast: Double

    public init(mean: Double, contrast: Double) {
        self.mean = mean
        self.contrast = contrast
    }
}

public struct FaceStreamCrop: Equatable, Sendable {
    public let x: Int
    public let y: Int
    public let w: Int
    public let h: Int
}

public struct FaceStreamPolicy: Equatable, Sendable {
    public var width = 256
    public var height = 320
    public var quality = 0.72
    public var minIntervalMs = 4000.0
    public var refreshMs = 10000.0
    public var better = 0.15
    public var maxYaw = 0.26
    public var maxPitch = 0.26
    public var maxRoll = 0.35
    public var maxLid = 0.2
    public var maxMouth = 0.12
    public var luma = (0.22, 0.85)
    public var minContrast = 0.035
    public var minSpan = 36.0
    public var maskW = 96
    public var maskH = 120
    public var cropHeight = 4.6
    public var cropDrop = 0.35
    public var restRise = 0.006
    public var eyeOpen = 0.25
    public var eyeFall = 0.0005
    public var budgetBurst = 10500.0
    public var budgetRate = 1400.0
    public var expectReference = 9000.0
    public var expectAtlas = 2500.0

    public init() {}

    public static func == (a: FaceStreamPolicy, b: FaceStreamPolicy) -> Bool {
        a.width == b.width && a.height == b.height && a.minIntervalMs == b.minIntervalMs && a.refreshMs == b.refreshMs && a.luma == b.luma
    }
}

public struct FaceStreamAtlasPolicy: Equatable, Sendable {
    public var tileW = 64
    public var tileH = 48
    public var quality = 0.82
    public var reach = 1.6
    public var minIntervalMs = 3000.0
    public var refreshMs = 60000.0
    public var better = 0.15
    public var maxYaw = 0.35
    public var maxPitch = 0.35
    public var maxRoll = 0.4
    public var minWidth = 20.0
    public var closed = 0.05
    public var open = 0.25
    public var mid = 0.1
    public var wide = 0.06
    public var round = 0.06
    public var restRate = 0.05

    public init() {}
}

public struct FaceStreamMouthCrop: Equatable, Sendable {
    public let center: CallFacePoint
    public let ux: CallFacePoint
    public let uy: CallFacePoint
    public let width: Double
    public let scale: Double
}

public struct FaceStreamMouthPick: Equatable, Sendable {
    public let state: Int
    public let score: Double
}

public struct FaceStreamMouthCandidate: Equatable, Sendable {
    public let time: Double
    public let pick: FaceStreamMouthPick
    public let crop: FaceStreamMouthCrop
    public let mouth: Double
    public let smile: Double
    public let points: [CallFacePoint]
}

public struct FaceStreamMouthFrame: Equatable, Sendable {
    public let center: CallFacePoint
    public let x: CallFacePoint
    public let y: CallFacePoint

    public init(center: CallFacePoint, x: CallFacePoint, y: CallFacePoint) {
        self.center = center
        self.x = x
        self.y = y
    }
}

public enum FaceStreamSending {
    public static func quatFromRows(_ r: [[Double]]) -> [Double] {
        let trace = r[0][0] + r[1][1] + r[2][2]
        var q: [Double]
        if trace > 0 {
            let s = (trace + 1).squareRoot() * 2
            q = [s / 4, (r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s]
        } else if r[0][0] > r[1][1] && r[0][0] > r[2][2] {
            let s = (1 + r[0][0] - r[1][1] - r[2][2]).squareRoot() * 2
            q = [(r[2][1] - r[1][2]) / s, s / 4, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s]
        } else if r[1][1] > r[2][2] {
            let s = (1 + r[1][1] - r[0][0] - r[2][2]).squareRoot() * 2
            q = [(r[0][2] - r[2][0]) / s, (r[0][1] + r[1][0]) / s, s / 4, (r[1][2] + r[2][1]) / s]
        } else {
            let s = (1 + r[2][2] - r[0][0] - r[1][1]).squareRoot() * 2
            q = [(r[1][0] - r[0][1]) / s, (r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, s / 4]
        }
        let n = q.reduce(0) { $0 + $1 * $1 }.squareRoot()
        let sign: Double = q[0] < 0 ? -1 : 1
        return q.map { sign * $0 / (n > 0 ? n : 1) }
    }

    public static func pose(_ points: [CallFacePoint]) -> FaceStreamSolvedPose {
        let solved = CallFaceGeometry.solvePose(FaceStream.templateModel, points)
        return FaceStreamSolvedPose(
            pose: FaceStreamPose(rotation: quatFromRows(solved.rows), x: solved.t[0], y: solved.t[1], scale: solved.scale),
            yaw: solved.yaw, pitch: solved.pitch, roll: solved.roll)
    }

    static func clamp(_ v: Double, _ low: Double, _ high: Double) -> Double { min(max(v, low), high) }

    public static func smile(_ points: [CallFacePoint]) -> Double {
        let frame = CallFaceGeometry.faceFrame(points)
        let width = ((points[54].x - points[48].x) * (points[54].x - points[48].x) + (points[54].y - points[48].y) * (points[54].y - points[48].y)).squareRoot() / frame.span
        return clamp((width - 0.96) / 0.3, 0, 1)
    }

    public static func expression(_ reading: CallFaceExpressionReading?, points: [CallFacePoint]) -> FaceStreamExpression {
        let close = reading?.close ?? [0, 0]
        let look = reading?.look ?? []
        func pick(_ side: Int, _ axis: Int) -> Double {
            guard side < look.count else { return 0 }
            let v = axis == 0 ? look[side].x : look[side].y
            return v.isFinite ? v : 0
        }
        return FaceStreamExpression(
            lidLeft: clamp(close.first ?? 0, 0, 1), lidRight: clamp(close.count > 1 ? close[1] : 0, 0, 1),
            gazeX: (pick(0, 0) + pick(1, 0)) / 2, gazeY: (pick(0, 1) + pick(1, 1)) / 2,
            mouth: clamp(CallFaceGeometry.mouthGap(points) / 0.5, 0, 1), smile: smile(points), tongue: clamp(reading?.tongue ?? 0, 0, 1))
    }

    public static func score(pose: FaceStreamSolvedPose, expression: FaceStreamExpression, light: FaceStreamLight?, span: Double, policy: FaceStreamPolicy = FaceStreamPolicy()) -> Double {
        guard span >= policy.minSpan else { return 0 }
        let turn = max(abs(pose.yaw) / policy.maxYaw, abs(pose.pitch) / policy.maxPitch, abs(pose.roll) / policy.maxRoll)
        let lids = max(expression.lidLeft, expression.lidRight) / policy.maxLid
        let mouth = expression.mouth / policy.maxMouth
        if turn >= 1 || lids >= 1 || mouth >= 1 || expression.tongue > 0.1 { return 0 }
        var lit = 1.0
        if let light {
            if light.mean < policy.luma.0 || light.mean > policy.luma.1 || light.contrast < policy.minContrast { return 0 }
            lit = 1 - abs(light.mean - 0.5) / 0.5
        }
        return clamp(0.45 * (1 - turn) + 0.2 * (1 - lids) + 0.2 * (1 - mouth) + 0.15 * lit, 0, 1)
    }

    public static func due(lastTime: Double?, lastScore: Double, time: Double, score: Double, policy: FaceStreamPolicy = FaceStreamPolicy()) -> Bool {
        guard score > 0 else { return false }
        guard let lastTime else { return true }
        let age = time - lastTime
        if age < policy.minIntervalMs { return false }
        if age >= policy.refreshMs { return true }
        return score > lastScore + policy.better
    }

    public static func crop(_ points: [CallFacePoint], frameWidth: Int, frameHeight: Int, policy: FaceStreamPolicy = FaceStreamPolicy()) -> FaceStreamCrop {
        let frame = CallFaceGeometry.faceFrame(points)
        let aspect = Double(policy.width) / Double(policy.height)
        var h = policy.cropHeight * frame.span
        var w = h * aspect
        let fit = min(1, Double(frameWidth) / w, Double(frameHeight) / h)
        w = max(1, (w * fit).rounded(.down))
        h = max(1, (h * fit).rounded(.down))
        let cx = frame.mid.x + frame.uy.x * policy.cropDrop * frame.span
        let cy = frame.mid.y + frame.uy.y * policy.cropDrop * frame.span
        let x = Int(clamp((cx - w / 2).rounded(.toNearestOrAwayFromZero), 0, max(0, Double(frameWidth) - w)))
        let y = Int(clamp((cy - h / 2).rounded(.toNearestOrAwayFromZero), 0, max(0, Double(frameHeight) - h)))
        return FaceStreamCrop(x: x, y: y, w: Int(w), h: Int(h))
    }

    public static func referencePoints(_ points: [CallFacePoint], crop: FaceStreamCrop, width: Int, height: Int) -> [CallFacePoint] {
        points.prefix(68).map { CallFacePoint(($0.x - Double(crop.x)) * (Double(width) / Double(crop.w)), ($0.y - Double(crop.y)) * (Double(height) / Double(crop.h))) }
    }

    public static func maskBits(_ bytes: [UInt8], maskWidth: Int, maskHeight: Int, frameWidth: Int, frameHeight: Int, crop: FaceStreamCrop, w: Int, h: Int) -> [UInt8] {
        let sx = Double(maskWidth) / Double(frameWidth)
        let sy = Double(maskHeight) / Double(frameHeight)
        var out = [UInt8](repeating: 0, count: w * h)
        for y in 0..<h {
            for x in 0..<w {
                let fx = Double(crop.x) + (Double(x) + 0.5) / Double(w) * Double(crop.w)
                let fy = Double(crop.y) + (Double(y) + 0.5) / Double(h) * Double(crop.h)
                let mx = min(max(Int((fx * sx).rounded(.down)), 0), maskWidth - 1)
                let my = min(max(Int((fy * sy).rounded(.down)), 0), maskHeight - 1)
                out[y * w + x] = bytes[my * maskWidth + mx] >= 128 ? 1 : 0
            }
        }
        return out
    }

    public static func eyeOpening(_ points: [CallFacePoint]) -> Double {
        func d(_ a: Int, _ b: Int) -> Double { ((points[a].x - points[b].x) * (points[a].x - points[b].x) + (points[a].y - points[b].y) * (points[a].y - points[b].y)).squareRoot() }
        func eye(_ o: Int) -> Double { (d(o + 1, o + 5) + d(o + 2, o + 4)) / (2 * max(d(o, o + 3), 1e-6)) }
        return (eye(36) + eye(42)) / 2
    }

    public static func mouthWidth(_ points: [CallFacePoint]) -> Double {
        ((points[54].x - points[48].x) * (points[54].x - points[48].x) + (points[54].y - points[48].y) * (points[54].y - points[48].y)).squareRoot()
            / CallFaceGeometry.faceFrame(points).span
    }

    public static func lipRatio(_ points: [CallFacePoint], offset: Int = 0) -> Double {
        func at(_ i: Int) -> CallFacePoint { points[i - offset] }
        let width = ((at(64).x - at(60).x) * (at(64).x - at(60).x) + (at(64).y - at(60).y) * (at(64).y - at(60).y)).squareRoot()
        let gap = ((at(66).x - at(62).x) * (at(66).x - at(62).x) + (at(66).y - at(62).y) * (at(66).y - at(62).y)).squareRoot()
        return width > 0 ? gap / width : 0
    }

    public static func mouthTileCrop(_ points: [CallFacePoint], policy: FaceStreamAtlasPolicy = FaceStreamAtlasPolicy()) -> FaceStreamMouthCrop {
        var cx = 0.0, cy = 0.0
        for i in 48..<60 {
            cx += points[i].x / 12
            cy += points[i].y / 12
        }
        let dx = points[54].x - points[48].x
        let dy = points[54].y - points[48].y
        let width = (dx * dx + dy * dy).squareRoot()
        let ux = width > 0 ? CallFacePoint(dx / width, dy / width) : CallFacePoint(1, 0)
        return FaceStreamMouthCrop(center: CallFacePoint(cx, cy), ux: ux, uy: CallFacePoint(-ux.y, ux.x), width: width,
            scale: max(1e-6, policy.reach * width / Double(policy.tileW)))
    }

    public static func mouthTilePoints(_ points: [CallFacePoint], crop: FaceStreamMouthCrop, policy: FaceStreamAtlasPolicy = FaceStreamAtlasPolicy()) -> [CallFacePoint] {
        (48..<68).map { i in
            let dx = points[i].x - crop.center.x
            let dy = points[i].y - crop.center.y
            return CallFacePoint((dx * crop.ux.x + dy * crop.ux.y) / crop.scale + Double(policy.tileW) / 2, (dx * crop.uy.x + dy * crop.uy.y) / crop.scale + Double(policy.tileH) / 2)
        }
    }

    public static func mouthState(pose: FaceStreamSolvedPose, expression: FaceStreamExpression, width: Double?, restWidth: Double?,
                                  policy: FaceStreamAtlasPolicy = FaceStreamAtlasPolicy()) -> FaceStreamMouthPick? {
        let turn = max(abs(pose.yaw) / policy.maxYaw, abs(pose.pitch) / policy.maxPitch, abs(pose.roll) / policy.maxRoll)
        if turn >= 1 { return nil }
        let m = expression.mouth
        var s = 0.0
        if let width, let restWidth { s = width / restWidth - 1 }
        let known = width != nil && restWidth != nil
        let state: Int
        let fit: Double
        if m <= policy.closed {
            state = FaceStreamAtlas.closed
            fit = 1 - m / policy.closed
        } else if m >= policy.open {
            state = FaceStreamAtlas.open
            fit = min(1, 0.5 + (m - policy.open) / 0.4)
        } else if m >= policy.mid && known && s >= policy.wide {
            state = FaceStreamAtlas.wide
            fit = min(1, 0.5 + (s - policy.wide) / 0.1)
        } else if m >= policy.mid && known && s <= -policy.round {
            state = FaceStreamAtlas.round
            fit = min(1, 0.5 + (-s - policy.round) / 0.1)
        } else {
            return nil
        }
        return FaceStreamMouthPick(state: state, score: clamp(fit, 0, 1) * (1 - 0.5 * turn))
    }

    public static func light(rgba: [UInt8], width: Int, height: Int, points: [CallFacePoint]) -> FaceStreamLight {
        let xs = [36, 39, 42, 45, 48, 54].map { points[$0].x }
        let ys = [19, 24, 33, 57].map { points[$0].y }
        let x0 = min(max(Int((xs.min() ?? 0).rounded(.down)), 0), width - 1)
        let x1 = min(max(Int((xs.max() ?? 0).rounded(.up)), 0), width - 1)
        let y0 = min(max(Int((ys.min() ?? 0).rounded(.down)), 0), height - 1)
        let y1 = min(max(Int((ys.max() ?? 0).rounded(.up)), 0), height - 1)
        let stepX = max(1, (x1 - x0) / 24)
        let stepY = max(1, (y1 - y0) / 24)
        var sum = 0.0, squares = 0.0, n = 0.0
        var y = y0
        while y <= y1 {
            var x = x0
            while x <= x1 {
                let at = (y * width + x) * 4
                let luma = (0.299 * Double(rgba[at]) + 0.587 * Double(rgba[at + 1]) + 0.114 * Double(rgba[at + 2])) / 255
                sum += luma
                squares += luma * luma
                n += 1
                x += stepX
            }
            y += stepY
        }
        guard n > 0 else { return FaceStreamLight(mean: 0, contrast: 0) }
        let mean = sum / n
        return FaceStreamLight(mean: mean, contrast: max(0, squares / n - mean * mean).squareRoot())
    }
}

public final class FaceStreamSender {
    public let encoder: FaceStreamEncoder
    public let policy: FaceStreamPolicy
    public let atlasPolicy: FaceStreamAtlasPolicy
    private var lastTime: Double?
    private var lastScore = 0.0
    private var nextId = 0
    private var restMouth: Double?
    private var openMax: Double?
    private var restWidth: Double?
    private var credit: Double
    private var creditTime: Double?
    private var referenceSize: Double
    private var atlasSize: Double
    private var tiles: [Int: (candidate: FaceStreamMouthCandidate, rgba: [UInt8])] = [:]
    private var atlasId = 0
    private var atlasTime: Double?
    private var atlasDirty = false

    public init(_ config: FaceStreamConfig, policy: FaceStreamPolicy = FaceStreamPolicy(), atlasPolicy: FaceStreamAtlasPolicy = FaceStreamAtlasPolicy()) throws {
        encoder = try FaceStreamEncoder(config)
        self.policy = policy
        self.atlasPolicy = atlasPolicy
        credit = policy.budgetBurst
        referenceSize = policy.expectReference
        atlasSize = policy.expectAtlas
    }

    public var budget: Double { credit }
    public var tileCount: Int { tiles.count }

    private func refill(_ time: Double) -> Double {
        if let creditTime { credit = min(policy.budgetBurst, credit + policy.budgetRate * max(0, time - creditTime) / 1000) }
        creditTime = time
        return credit
    }

    public func frame(time: UInt32, points: [CallFacePoint], reading: CallFaceExpressionReading?) throws -> [UInt8] {
        let solved = FaceStreamSending.pose(points)
        return try encoder.encode(FaceStreamInput(time: time, points: points, pose: solved.pose, expression: FaceStreamSending.expression(reading, points: points)))
    }

    public func score(points: [CallFacePoint], reading: CallFaceExpressionReading?, light: FaceStreamLight?) -> Double {
        var expression = FaceStreamSending.expression(reading, points: points)
        let rest = restMouth.map { min(expression.mouth, $0 + policy.restRise) } ?? 0
        restMouth = rest
        let opening = FaceStreamSending.eyeOpening(points)
        let top = openMax.map { max(opening, $0 - policy.eyeFall) } ?? max(opening, policy.eyeOpen)
        openMax = top
        let shut = FaceStreamSending.clamp(1 - opening / top, 0, 1)
        expression.mouth = max(0, expression.mouth - rest)
        expression.lidLeft = max(expression.lidLeft, shut)
        expression.lidRight = max(expression.lidRight, shut)
        return FaceStreamSending.score(pose: FaceStreamSending.pose(points), expression: expression, light: light,
            span: CallFaceGeometry.faceFrame(points).span, policy: policy)
    }

    public func due(time: Double, score: Double) -> Bool {
        FaceStreamSending.due(lastTime: lastTime, lastScore: lastScore, time: time, score: score, policy: policy) && refill(time) >= min(referenceSize, policy.budgetBurst)
    }

    public func crop(_ points: [CallFacePoint]) -> FaceStreamCrop {
        FaceStreamSending.crop(points, frameWidth: encoder.config.width, frameHeight: encoder.config.height, policy: policy)
    }

    public func reference(time: Double, crop: FaceStreamCrop, points: [CallFacePoint], format: Int, image: [UInt8], mask: [UInt8]?, score: Double) throws -> [UInt8] {
        let reference = FaceStreamReference(
            id: nextId, format: format, time: UInt32(truncatingIfNeeded: Int64(time)), cropX: crop.x, cropY: crop.y, cropW: crop.w, cropH: crop.h,
            width: policy.width, height: policy.height, points: FaceStreamSending.referencePoints(points, crop: crop, width: policy.width, height: policy.height),
            maskW: mask == nil ? 0 : policy.maskW, maskH: mask == nil ? 0 : policy.maskH, mask: mask, image: image)
        let packet = try reference.encoded()
        nextId = (nextId + 1) & 15
        lastTime = time
        lastScore = score
        _ = refill(time)
        credit -= Double(packet.count)
        referenceSize = Double(packet.count)
        return packet
    }

    public func mouthCandidate(time: Double, points: [CallFacePoint], reading: CallFaceExpressionReading?) -> FaceStreamMouthCandidate? {
        let solved = FaceStreamSending.pose(points)
        let expression = FaceStreamSending.expression(reading, points: points)
        let width = FaceStreamSending.mouthWidth(points)
        if expression.mouth <= atlasPolicy.closed { restWidth = restWidth.map { $0 + atlasPolicy.restRate * (width - $0) } ?? width }
        guard let pick = FaceStreamSending.mouthState(pose: solved, expression: expression, width: width, restWidth: restWidth, policy: atlasPolicy),
            pick.score > 0
        else { return nil }
        let crop = FaceStreamSending.mouthTileCrop(points, policy: atlasPolicy)
        guard crop.width >= atlasPolicy.minWidth else { return nil }
        if let have = tiles[pick.state]?.candidate, time - have.time < atlasPolicy.refreshMs, pick.score <= have.pick.score + atlasPolicy.better { return nil }
        return FaceStreamMouthCandidate(time: time, pick: pick, crop: crop, mouth: expression.mouth, smile: expression.smile,
            points: FaceStreamSending.mouthTilePoints(points, crop: crop, policy: atlasPolicy))
    }

    public func storeMouthTile(_ candidate: FaceStreamMouthCandidate, rgba: [UInt8]) {
        guard rgba.count >= atlasPolicy.tileW * atlasPolicy.tileH * 4 else { return }
        tiles[candidate.pick.state] = (candidate, rgba)
        if candidate.pick.state != FaceStreamAtlas.closed || tiles.count > 1 { atlasDirty = true }
    }

    public func atlasDue(time: Double) -> Bool {
        let useful = tiles.keys.contains { $0 != FaceStreamAtlas.closed }
        return atlasDirty && useful && (atlasTime.map { time - $0 >= atlasPolicy.minIntervalMs } ?? true) && refill(time) >= atlasSize
    }

    public func atlasSheet() -> (tiles: [FaceStreamAtlasTile], rgba: [UInt8], width: Int, height: Int) {
        let list = tiles.keys.sorted().compactMap { tiles[$0] }
        let tw = atlasPolicy.tileW, th = atlasPolicy.tileH
        let width = tw * list.count
        var rgba = [UInt8](repeating: 255, count: width * th * 4)
        for (k, entry) in list.enumerated() {
            for y in 0..<th {
                for x in 0..<tw {
                    for c in 0..<4 { rgba[(y * width + k * tw + x) * 4 + c] = entry.rgba[(y * tw + x) * 4 + c] }
                }
            }
        }
        let meta = list.map { FaceStreamAtlasTile(state: $0.candidate.pick.state, mouth: $0.candidate.mouth, smile: $0.candidate.smile, points: $0.candidate.points) }
        return (meta, rgba, width, th)
    }

    public func atlas(time: Double, tiles meta: [FaceStreamAtlasTile], image: [UInt8], format: Int = FaceStreamReference.jpeg) throws -> [UInt8] {
        let packet = try FaceStreamAtlas(id: atlasId, time: UInt32(truncatingIfNeeded: Int64(time)), tileW: atlasPolicy.tileW, tileH: atlasPolicy.tileH,
            format: format, tiles: meta, image: image).encoded()
        atlasId = (atlasId + 1) & 15
        atlasTime = time
        atlasDirty = false
        _ = refill(time)
        credit -= Double(packet.count)
        atlasSize = Double(packet.count)
        return packet
    }

    public func requestKey() { encoder.requestKey() }
}

public enum FaceStreamResult: Equatable {
    case frame(FaceStreamFrame)
    case reference(FaceStreamReference)
    case atlas(FaceStreamAtlas)
    case failed(String)
}

public final class FaceStreamReceiver {
    private let decoder = FaceStreamDecoder()
    public private(set) var frame: FaceStreamFrame?
    public private(set) var reference: FaceStreamReference?
    public private(set) var atlas: FaceStreamAtlas?
    public private(set) var errors = 0
    public private(set) var needsKey = false
    private var goodMouth = Double.nan

    public init() {}

    @discardableResult
    public func receive(_ packet: [UInt8]) -> FaceStreamResult {
        do {
            if FaceStream.packetKind(packet) == FaceStream.kindReference {
                let r = try FaceStreamReference.decode(packet)
                reference = r
                return .reference(r)
            }
            if FaceStream.packetKind(packet) == FaceStream.kindAtlas {
                let a = try FaceStreamAtlas.decode(packet)
                atlas = a
                return .atlas(a)
            }
            let decoded = try decoder.decode(packet)
            if !decoded.stale { goodMouth = decoded.expression.mouth }
            needsKey = decoded.stale
            let f = FaceStreamView.conceal(decoded, goodMouth: goodMouth)
            frame = f
            return .frame(f)
        } catch let error as FaceStreamError {
            errors += 1
            return .failed(error.code)
        } catch {
            errors += 1
            return .failed("\(error)")
        }
    }

    public func livePoints(width: Double, height: Double) -> [CallFacePoint]? {
        guard let frame, let reference else { return nil }
        return FaceStreamView.livePoints(frame, reference: reference, width: width, height: height)
    }
}

public enum FaceStreamView {
    static let concealMouth: [(Int, Double)] = [(56, 1), (57, 1), (58, 1), (65, 1), (66, 1), (67, 1), (55, 0.6), (59, 0.6), (7, 0.7), (8, 0.8), (9, 0.7), (6, 0.4), (10, 0.4)]

    public static func conceal(_ frame: FaceStreamFrame, goodMouth: Double) -> FaceStreamFrame {
        guard frame.stale, goodMouth.isFinite else { return frame }
        let delta = (frame.expression.mouth - goodMouth) * 0.5
        guard abs(delta) >= 1e-6 else { return frame }
        let r = FaceStream.rotationRows(frame.pose.rotation)
        let down = (frame.pose.scale * r[0][1], frame.pose.scale * r[1][1])
        var out = frame
        for (i, weight) in concealMouth {
            out.points[i] = CallFacePoint(out.points[i].x + down.0 * delta * weight, out.points[i].y + down.1 * delta * weight)
        }
        return out
    }

    public static func livePoints(_ frame: FaceStreamFrame, reference: FaceStreamReference, width: Double, height: Double) -> [CallFacePoint] {
        let sx = width / Double(reference.cropW)
        let sy = height / Double(reference.cropH)
        let r3 = FaceStream.rotationRows(frame.pose.rotation)[2]
        let depth = frame.pose.scale * (sx + sy) * 0.5
        return frame.points.enumerated().map { i, p in
            CallFacePoint((p.x - Double(reference.cropX)) * sx, (p.y - Double(reference.cropY)) * sy, depth * FaceStream.dot3(r3, FaceStream.template[i]))
        }
    }

    public static func mouthFrame(_ points: [CallFacePoint]) -> FaceStreamMouthFrame {
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
        let ux = (dx / width, dy / width)
        let uy = (-ux.1, ux.0)
        let gap = abs((points[66].x - points[62].x) * uy.0 + (points[66].y - points[62].y) * uy.1)
        let halfX = width / 2
        let halfY = max(gap / 2, 0.04 * frame.span)
        return FaceStreamMouthFrame(center: CallFacePoint(cx, cy), x: CallFacePoint(ux.0 / halfX, ux.1 / halfX), y: CallFacePoint(uy.0 / halfY, uy.1 / halfY))
    }

    static func smoothstep(_ a: Double, _ b: Double, _ x: Double) -> Double {
        let t = min(max((x - a) / (b - a), 0), 1)
        return t * t * (3 - 2 * t)
    }

    public static func removal(_ reference: FaceStreamReference) -> [Float] {
        let w = reference.width, h = reference.height
        var out = [Float](repeating: 0, count: w * h)
        guard let mask = reference.mask, reference.maskW > 0 else { return out }
        let frame = CallFaceGeometry.faceFrame(reference.points)
        for y in 0..<h {
            for x in 0..<w {
                let dx = Double(x) + 0.5 - frame.mid.x
                let dy = Double(y) + 0.5 - frame.mid.y
                let qx = (dx * frame.ux.x + dy * frame.ux.y) / frame.span
                let qy = (dx * frame.uy.x + dy * frame.uy.y) / frame.span
                let reach = ((qx / 2.55) * (qx / 2.55) + ((qy + 0.35) / 2.9) * ((qy + 0.35) / 2.9)).squareRoot()
                let zone = 1 - smoothstep(0.85, 1.1, reach)
                let above = 1 - smoothstep(1.3, 1.8, qy)
                let mx = min(Int((Double(x) + 0.5) / Double(w) * Double(reference.maskW)), reference.maskW - 1)
                let my = min(Int((Double(y) + 0.5) / Double(h) * Double(reference.maskH)), reference.maskH - 1)
                out[y * w + x] = Float(Double(mask[my * reference.maskW + mx]) * zone * above)
            }
        }
        return spread(out, w, h)
    }

    static func spread(_ values: [Float], _ w: Int, _ h: Int) -> [Float] {
        let blurred = blur(values, w, h, 3)
        return zip(values, blurred).map { max($0, min(1, $1 * 1.6)) }
    }

    static func blur(_ values: [Float], _ w: Int, _ h: Int, _ radius: Int) -> [Float] {
        var tmp = [Float](repeating: 0, count: values.count)
        var out = [Float](repeating: 0, count: values.count)
        let span = Float(2 * radius + 1)
        for y in 0..<h {
            for x in 0..<w {
                var sum: Float = 0
                for k in -radius...radius { sum += values[y * w + min(max(x + k, 0), w - 1)] }
                tmp[y * w + x] = sum / span
            }
        }
        for y in 0..<h {
            for x in 0..<w {
                var sum: Float = 0
                for k in -radius...radius { sum += tmp[min(max(y + k, 0), h - 1) * w + x] }
                out[y * w + x] = sum / span
            }
        }
        return out
    }

    static func pushPull(rgba: [UInt8], width w: Int, height h: Int, removal: [Float]) -> [Float] {
        struct Level {
            var data: [Float]
            let w: Int
            let h: Int
        }
        var first = [Float](repeating: 0, count: w * h * 4)
        for i in 0..<(w * h) {
            let keep = Float(pow(Double(1 - min(max(removal[i], 0), 1)), 3))
            for c in 0..<3 { first[i * 4 + c] = Float(rgba[i * 4 + c]) / 255 * keep }
            first[i * 4 + 3] = keep
        }
        var levels = [Level(data: first, w: w, h: h)]
        while let top = levels.last, top.w > 1 || top.h > 1 {
            let nw = max(1, (top.w + 1) / 2), nh = max(1, (top.h + 1) / 2)
            var next = [Float](repeating: 0, count: nw * nh * 4)
            for y in 0..<nh {
                for x in 0..<nw {
                    for k in 0..<4 {
                        let sx = min(top.w - 1, x * 2 + (k & 1))
                        let sy = min(top.h - 1, y * 2 + (k >> 1))
                        for c in 0..<4 { next[(y * nw + x) * 4 + c] += top.data[(sy * top.w + sx) * 4 + c] / 4 }
                    }
                }
            }
            levels.append(Level(data: next, w: nw, h: nh))
        }
        var coarse: Level?
        for level in levels.reversed() {
            var filled = [Float](repeating: 1, count: level.w * level.h * 4)
            for y in 0..<level.h {
                for x in 0..<level.w {
                    let at = (y * level.w + x) * 4
                    let a = level.data[at + 3]
                    let trust: Float = coarse == nil ? 1 : min(max(a * 3, 0), 1)
                    var x0 = 0, y0 = 0, x1 = 0, y1 = 0
                    var fx: Float = 0, fy: Float = 0
                    if let coarse {
                        let sx = min(max((Float(x) + 0.5) / 2 - 0.5, 0), Float(coarse.w - 1))
                        let sy = min(max((Float(y) + 0.5) / 2 - 0.5, 0), Float(coarse.h - 1))
                        x0 = Int(sx.rounded(.down))
                        y0 = Int(sy.rounded(.down))
                        fx = sx - Float(x0)
                        fy = sy - Float(y0)
                        x1 = min(coarse.w - 1, x0 + 1)
                        y1 = min(coarse.h - 1, y0 + 1)
                    }
                    for c in 0..<3 {
                        let mine: Float = a > 1e-4 ? level.data[at + c] / a : 0.5
                        var under = mine
                        if let coarse {
                            func tap(_ px: Int, _ py: Int) -> Float { coarse.data[(py * coarse.w + px) * 4 + c] }
                            under = (tap(x0, y0) * (1 - fx) + tap(x1, y0) * fx) * (1 - fy) + (tap(x0, y1) * (1 - fx) + tap(x1, y1) * fx) * fy
                        }
                        filled[at + c] = under + (mine - under) * trust
                    }
                }
            }
            coarse = Level(data: filled, w: level.w, h: level.h)
        }
        return coarse?.data ?? first
    }

    static func planes(_ data: [Float], _ w: Int, _ h: Int) -> [[Float]] {
        (0..<3).map { c in (0..<(w * h)).map { data[$0 * 4 + c] } }
    }

    public static func backdrop(rgba: [UInt8], width w: Int, height h: Int, removal: [Float], blurRadius: Int = 2) -> [UInt8] {
        var layers = planes(pushPull(rgba: rgba, width: w, height: h, removal: removal), w, h)
        if blurRadius > 0 { layers = layers.map { blur(blur($0, w, h, blurRadius), w, h, blurRadius) } }
        var out = [UInt8](repeating: 255, count: w * h * 4)
        for i in 0..<(w * h) {
            for c in 0..<3 { out[i * 4 + c] = UInt8(min(max((layers[c][i] * 255).rounded(), 0), 255)) }
        }
        return out
    }
}
