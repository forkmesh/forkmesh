import Foundation

public enum FaceStreamError: Error, Equatable {
    case short
    case long
    case version
    case kind
    case header
    case needKey
    case old
    case input
    case config
    case corrupt
    case notAFrame
    case notAReference
    case notAnAtlas

    public var code: String {
        switch self {
        case .short: return "short"
        case .long: return "long"
        case .version: return "version"
        case .kind: return "kind"
        case .header: return "header"
        case .needKey: return "need-key"
        case .old: return "old"
        case .input: return "input"
        case .config: return "config"
        case .corrupt: return "corrupt"
        case .notAFrame: return "not-a-frame"
        case .notAReference: return "not-a-reference"
        case .notAnAtlas: return "not-an-atlas"
        }
    }
}

public struct FaceStreamPose: Equatable, Sendable {
    public var rotation: [Double]
    public var x: Double
    public var y: Double
    public var scale: Double

    public init(rotation: [Double], x: Double, y: Double, scale: Double) {
        self.rotation = rotation
        self.x = x
        self.y = y
        self.scale = scale
    }
}

public struct FaceStreamExpression: Equatable, Sendable {
    public var lidLeft = 0.0
    public var lidRight = 0.0
    public var gazeX = 0.0
    public var gazeY = 0.0
    public var mouth = 0.0
    public var smile = 0.0
    public var tongue = 0.0

    public init(lidLeft: Double = 0, lidRight: Double = 0, gazeX: Double = 0, gazeY: Double = 0, mouth: Double = 0, smile: Double = 0, tongue: Double = 0) {
        self.lidLeft = lidLeft
        self.lidRight = lidRight
        self.gazeX = gazeX
        self.gazeY = gazeY
        self.mouth = mouth
        self.smile = smile
        self.tongue = tongue
    }

    public var values: [Double] { [lidLeft, lidRight, gazeX, gazeY, mouth, smile, tongue] }

    public init(values v: [Double]) {
        self.init(lidLeft: v[0], lidRight: v[1], gazeX: v[2], gazeY: v[3], mouth: v[4], smile: v[5], tongue: v[6])
    }
}

public struct FaceStreamInput: Equatable, Sendable {
    public var time: UInt32
    public var points: [CallFacePoint]
    public var pose: FaceStreamPose
    public var expression: FaceStreamExpression

    public init(time: UInt32, points: [CallFacePoint], pose: FaceStreamPose, expression: FaceStreamExpression) {
        self.time = time
        self.points = points
        self.pose = pose
        self.expression = expression
    }
}

public struct FaceStreamFrame: Equatable, Sendable {
    public var key: Bool
    public var stale: Bool
    public var seq: Int
    public var keyId: Int
    public var time: UInt32
    public var width: Int
    public var height: Int
    public var precision: Int
    public var pose: FaceStreamPose
    public var expression: FaceStreamExpression
    public var points: [CallFacePoint]
}

public struct FaceStreamConfig: Equatable, Sendable {
    public var width: Int
    public var height: Int
    public var fps: Int
    public var precision: Int
    public var keyInterval: Int

    public init(width: Int, height: Int, fps: Int = 0, precision: Int = 0, keyInterval: Int = 0) {
        self.width = width
        self.height = height
        self.fps = fps
        self.precision = precision
        self.keyInterval = keyInterval
    }

    func normalized(strict: Bool = false) -> FaceStreamConfig? {
        var c = self
        if strict && (c.fps == 0 || c.precision == 0) { return nil }
        if c.fps == 0 { c.fps = FaceStream.defaultFPS }
        if c.precision == 0 { c.precision = FaceStream.defaultPrecision }
        if c.keyInterval == 0 { c.keyInterval = FaceStream.defaultKeyInterval }
        guard c.width >= 1, c.width <= FaceStream.maxSide, c.height >= 1, c.height <= FaceStream.maxSide, c.fps >= 1, c.fps <= 120,
            c.precision >= FaceStream.minPrecision, c.precision <= FaceStream.maxPrecision, c.keyInterval >= 1, c.keyInterval <= 255
        else { return nil }
        return c
    }
}

public enum FaceStream {
    public static let version = 1
    public static let kindDelta = 0
    public static let kindKey = 1
    public static let kindReference = 2
    public static let kindAtlas = 3
    public static let defaultPrecision = 96
    public static let defaultKeyInterval = 30
    public static let defaultFPS = 30
    public static let maxFramePacket = 1024
    public static let minPrecision = 16
    public static let maxPrecision = 255
    public static let maxSide = 4096
    public static let landmarkCount = 68
    public static let label = "Face stream"
    public static let detail = "Animated from a recent photo while the connection is weak"
    public static let staleDetail = "Face stream: catching up"
    static let keyHeader = 12
    static let quatSteps = 512.0
    static let transSteps = 2.0
    static let scaleSteps = 8.0
    static let maxTimeJump = 4000

    public static let template: [[Double]] = [
        [-1.0496, 0.0765, -0.95], [-1.0473, 0.3567, -0.9], [-1.032, 0.6366, -0.8], [-0.9939, 0.914, -0.65],
        [-0.9103, 1.1805, -0.45], [-0.7622, 1.4155, -0.25], [-0.5366, 1.5709, -0.08], [-0.2751, 1.6607, 0.05],
        [0, 1.7019, 0.1], [0.2751, 1.6607, 0.05], [0.5366, 1.5709, -0.08], [0.7622, 1.4155, -0.25],
        [0.9103, 1.1805, -0.45], [0.9939, 0.914, -0.65], [1.032, 0.6366, -0.8], [1.0473, 0.3567, -0.9],
        [1.0496, 0.0765, -0.95], [-0.8602, -0.2609, -0.25], [-0.7013, -0.3102, -0.08], [-0.5424, -0.3296, 0.02],
        [-0.3836, -0.3117, 0.05], [-0.2248, -0.3094, 0.05], [0.2248, -0.3094, 0.05], [0.3836, -0.3117, 0.05],
        [0.5424, -0.3296, 0.02], [0.7013, -0.3102, -0.08], [0.8602, -0.2609, -0.25], [0, -0.0138, 0],
        [0, 0.1764, 0.12], [0, 0.365, 0.25], [0, 0.554, 0.38], [-0.2372, 0.6484, 0.12],
        [-0.1297, 0.6943, 0.2], [0, 0.7127, 0.25], [0.1297, 0.6943, 0.2], [0.2372, 0.6484, 0.12],
        [-0.7439, -0.0089, -0.12], [-0.5813, -0.07, -0.04], [-0.4077, -0.0609, -0.04], [-0.2572, 0.0273, -0.03],
        [-0.4211, 0.0591, -0.04], [-0.5888, 0.0535, -0.04], [0.2572, 0.0273, -0.03], [0.4077, -0.0609, -0.04],
        [0.5813, -0.07, -0.04], [0.7439, -0.0089, -0.12], [0.5888, 0.0535, -0.04], [0.4211, 0.0591, -0.04],
        [-0.4809, 0.9814, 0.02], [-0.3228, 0.9409, 0.12], [-0.1623, 0.9114, 0.17], [0, 0.9221, 0.19],
        [0.1623, 0.9114, 0.17], [0.3228, 0.9409, 0.12], [0.4809, 0.9814, 0.02], [0.3384, 1.0858, 0.12],
        [0.1765, 1.1543, 0.15], [0, 1.1789, 0.16], [-0.1765, 1.1543, 0.15], [-0.3384, 1.0858, 0.12],
        [-0.4495, 0.9876, 0.04], [-0.2256, 1.021, 0.13], [0, 1.0353, 0.14], [0.2256, 1.021, 0.13],
        [0.4495, 0.9876, 0.04], [0.2261, 1.021, 0.13], [0, 1.0353, 0.14], [-0.2261, 1.021, 0.13],
    ]

    public static let templateModel: [CallFacePoint] = template.map { CallFacePoint($0[0], $0[1], $0[2]) }

    struct Channel {
        let steps: Double
        let low: Int
        let high: Int
        let signed: Bool
        let prior: UInt16
    }

    static let channels: [Channel] = [
        Channel(steps: 16, low: 0, high: 16, signed: false, prior: 3600),
        Channel(steps: 16, low: 0, high: 16, signed: false, prior: 3600),
        Channel(steps: 64, low: -32, high: 32, signed: true, prior: FaceStreamBits.probHalf),
        Channel(steps: 64, low: -32, high: 32, signed: true, prior: FaceStreamBits.probHalf),
        Channel(steps: 16, low: 0, high: 16, signed: false, prior: FaceStreamBits.probHalf),
        Channel(steps: 16, low: 0, high: 16, signed: false, prior: 2800),
        Channel(steps: 16, low: 0, high: 16, signed: false, prior: 3900),
    ]

    static func floorHalf(_ value: Double) -> Int {
        let f = (value + 0.5).rounded(.down)
        if f > 1e9 { return 1_000_000_000 }
        if f < -1e9 { return -1_000_000_000 }
        return Int(f)
    }

    static func group(_ i: Int) -> Int {
        if i <= 16 { return 0 }
        if i <= 26 { return 1 }
        if i <= 35 { return 2 }
        if i <= 47 { return 3 }
        if i <= 59 { return 4 }
        return 5
    }

    public static func rotationRows(_ q: [Double]) -> [[Double]] {
        let w = q[0], x = q[1], y = q[2], z = q[3]
        let a = w * w + x * x - y * y - z * z
        let b = 2 * (x * y - w * z)
        let c = 2 * (x * z + w * y)
        let d = 2 * (x * y + w * z)
        let e = w * w - x * x + y * y - z * z
        let f = 2 * (y * z - w * x)
        let g = 2 * (x * z - w * y)
        let h = 2 * (y * z + w * x)
        let k = w * w - x * x - y * y + z * z
        return [[a, b, c], [d, e, f], [g, h, k]]
    }

    static func dot3(_ r: [Double], _ v: [Double]) -> Double {
        let a = r[0] * v[0]
        let b = r[1] * v[1]
        let c = r[2] * v[2]
        return a + b + c
    }

    public static func project(_ pose: FaceStreamPose) -> [CallFacePoint] {
        let r = rotationRows(pose.rotation)
        return template.map { m in
            let px = pose.scale * dot3(r[0], m)
            let py = pose.scale * dot3(r[1], m)
            return CallFacePoint(pose.x + px, pose.y + py)
        }
    }

    static func nominal(_ fps: Int) -> Int { (1000 + fps / 2) / fps }

    static func header(_ kind: Int, _ id: Int) -> UInt8 { UInt8(kind << 6 | version << 4 | id & 15) }

    public static func packetKind(_ packet: [UInt8]) -> Int { packet.isEmpty ? -1 : Int(packet[0] >> 6) }
}

struct FaceStreamQPose: Equatable {
    var qx = 0, qy = 0, qz = 0, tx = 0, ty = 0, s = 0

    init(qx: Int, qy: Int, qz: Int, tx: Int, ty: Int, s: Int) {
        self.qx = qx
        self.qy = qy
        self.qz = qz
        self.tx = tx
        self.ty = ty
        self.s = s
    }

    init(_ p: FaceStreamPose) {
        let q = p.rotation
        let n = (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]).squareRoot()
        var x = 0.0, y = 0.0, z = 0.0
        if n > 0 {
            x = q[1] / n
            y = q[2] / n
            z = q[3] / n
            if q[0] < 0 {
                x = -x
                y = -y
                z = -z
            }
        }
        let steps = FaceStream.quatSteps
        qx = faceStreamClamp(FaceStream.floorHalf(x * steps), -512, 512)
        qy = faceStreamClamp(FaceStream.floorHalf(y * steps), -512, 512)
        qz = faceStreamClamp(FaceStream.floorHalf(z * steps), -512, 512)
        tx = faceStreamClamp(FaceStream.floorHalf(p.x * FaceStream.transSteps), -32768, 32767)
        ty = faceStreamClamp(FaceStream.floorHalf(p.y * FaceStream.transSteps), -32768, 32767)
        s = faceStreamClamp(FaceStream.floorHalf(p.scale * FaceStream.scaleSteps), 1, 32767)
    }

    var values: [Int] { [qx, qy, qz, tx, ty, s] }

    var pose: FaceStreamPose {
        let x = Double(qx) / FaceStream.quatSteps
        let y = Double(qy) / FaceStream.quatSteps
        let z = Double(qz) / FaceStream.quatSteps
        let ss = x * x + y * y + z * z
        let w = ss < 1 ? (1 - ss).squareRoot() : 0
        let n = (w * w + x * x + y * y + z * z).squareRoot()
        return FaceStreamPose(rotation: [w / n, x / n, y / n, z / n], x: Double(tx) / FaceStream.transSteps, y: Double(ty) / FaceStream.transSteps, scale: Double(s) / FaceStream.scaleSteps)
    }
}

func faceStreamQuantize(_ e: FaceStreamExpression) -> [Int] {
    zip(e.values, FaceStream.channels).map { value, c in faceStreamClamp(FaceStream.floorHalf(value * c.steps), c.low, c.high) }
}

func faceStreamExpression(_ q: [Int]) -> FaceStreamExpression {
    FaceStreamExpression(values: zip(q, FaceStream.channels).map { Double($0) / $1.steps })
}

func faceStreamModels(_ count: Int) -> [FaceStreamIntModel] {
    [FaceStreamIntModel](repeating: FaceStreamIntModel(), count: count)
}

public final class FaceStreamEncoder {
    public let config: FaceStreamConfig
    private var keyId = 15
    private var keyPose = FaceStreamQPose(qx: 0, qy: 0, qz: 0, tx: 0, ty: 0, s: 0)
    private var seq = 255
    private var time: UInt32 = 0
    private var chain = faceStreamModels(12)
    private var recon: [[Double]] = Array(repeating: [0, 0], count: 68)
    private var projected: [CallFacePoint] = Array(repeating: CallFacePoint(0, 0), count: 68)
    private var started = false
    private var sinceKey = 0
    private var forceKey = false
    public private(set) var last: FaceStreamFrame?

    public init(_ config: FaceStreamConfig) throws {
        guard let c = config.normalized() else { throw FaceStreamError.config }
        self.config = c
    }

    public func requestKey() { forceKey = true }

    static func valid(_ input: FaceStreamInput) -> Bool {
        guard input.points.count >= 68, input.pose.rotation.count == 4 else { return false }
        for p in input.points.prefix(68) where !p.x.isFinite || !p.y.isFinite { return false }
        let numbers = input.pose.rotation + [input.pose.x, input.pose.y, input.pose.scale] + input.expression.values
        return numbers.allSatisfy(\.isFinite)
    }

    public func encode(_ input: FaceStreamInput) throws -> [UInt8] {
        guard Self.valid(input) else { throw FaceStreamError.input }
        let nominal = FaceStream.nominal(config.fps)
        let dt = Int(Int32(bitPattern: input.time &- time))
        let key = !started || forceKey || sinceKey >= config.keyInterval || dt - nominal > FaceStream.maxTimeJump || dt - nominal < -FaceStream.maxTimeJump
        let qp = FaceStreamQPose(input.pose)
        let pose = qp.pose
        let now = FaceStream.project(pose)
        let nextSeq = (seq + 1) & 255
        let id = key ? (keyId + 1) & 15 : keyId
        var prefix: [UInt8] = [FaceStream.header(key ? FaceStream.kindKey : FaceStream.kindDelta, id), UInt8(nextSeq)]
        if key {
            prefix += [UInt8(config.width >> 8), UInt8(config.width & 255), UInt8(config.height >> 8), UInt8(config.height & 255), UInt8(config.fps), UInt8(config.precision)]
            prefix += [UInt8(input.time >> 24), UInt8((input.time >> 16) & 255), UInt8((input.time >> 8) & 255), UInt8(input.time & 255)]
        }
        var enc = FaceStreamRangeEncoder(prefix: prefix)
        if key {
            enc.direct(UInt32(qp.qx + 512), 11)
            enc.direct(UInt32(qp.qy + 512), 11)
            enc.direct(UInt32(qp.qz + 512), 11)
            enc.direct(UInt32(qp.tx + 32768), 16)
            enc.direct(UInt32(qp.ty + 32768), 16)
            enc.direct(UInt32(qp.s), 15)
        } else {
            var timeModel = FaceStreamIntModel()
            enc.int(&timeModel, 0, dt - nominal)
            var models = faceStreamModels(6)
            let base = keyPose.values
            for (i, v) in qp.values.enumerated() { enc.int(&models[i], 0, v - base[i]) }
        }
        let qx = faceStreamQuantize(input.expression)
        for (i, c) in FaceStream.channels.enumerated() {
            var model = FaceStreamIntModel(zeroPrior: c.prior)
            if c.signed { enc.int(&model, 0, qx[i]) } else { enc.uint(&model, 0, qx[i]) }
        }
        if key { chain = faceStreamModels(12) }
        var models = key ? faceStreamModels(12) : chain
        let step = pose.scale / Double(config.precision)
        var next: [[Double]] = Array(repeating: [0, 0], count: 68)
        var nonzero = [Int](repeating: 0, count: 12)
        for i in 0..<68 {
            for axis in 0..<2 {
                let current = axis == 0 ? now[i].x : now[i].y
                let before = axis == 0 ? projected[i].x : projected[i].y
                let pred = key ? current : recon[i][axis] + (current - before)
                let value = axis == 0 ? input.points[i].x : input.points[i].y
                let r = faceStreamClamp(FaceStream.floorHalf((value - pred) / step), -FaceStreamBits.maxInt, FaceStreamBits.maxInt)
                let cls = FaceStream.group(i) * 2 + axis
                enc.int(&models[cls], nonzero[cls], r)
                nonzero[cls] = r != 0 ? 1 : 0
                next[i][axis] = pred + Double(r) * step
            }
        }
        if !key { chain = models }
        let packet = enc.finish()
        if key {
            keyId = id
            keyPose = qp
            sinceKey = 0
            forceKey = false
        }
        started = true
        sinceKey += 1
        seq = nextSeq
        time = input.time
        recon = next
        projected = now
        last = FaceStreamFrame(key: key, stale: false, seq: nextSeq, keyId: id, time: input.time, width: config.width, height: config.height,
            precision: config.precision, pose: pose, expression: faceStreamExpression(qx), points: next.map { CallFacePoint($0[0], $0[1]) })
        return packet
    }
}

public final class FaceStreamDecoder {
    private var config = FaceStreamConfig(width: 1, height: 1)
    private var keyId = 0
    private var keyPose = FaceStreamQPose(qx: 0, qy: 0, qz: 0, tx: 0, ty: 0, s: 0)
    private var seq = 0
    private var time: UInt32 = 0
    private var chain = faceStreamModels(12)
    private var recon: [[Double]] = Array(repeating: [0, 0], count: 68)
    private var projected: [CallFacePoint] = Array(repeating: CallFacePoint(0, 0), count: 68)
    private var haveKey = false
    private var chainOK = false
    private var lastGood: [[Double]] = Array(repeating: [0, 0], count: 68)
    private var goodProjected: [CallFacePoint] = Array(repeating: CallFacePoint(0, 0), count: 68)

    public init() {}

    public var ready: Bool { haveKey }

    public func reset() {
        haveKey = false
        chainOK = false
    }

    public func decode(_ packet: [UInt8]) throws -> FaceStreamFrame {
        guard packet.count >= 2 else { throw FaceStreamError.short }
        guard packet.count <= FaceStream.maxFramePacket else { throw FaceStreamError.long }
        guard Int(packet[0] >> 4) & 3 == FaceStream.version else { throw FaceStreamError.version }
        let kind = Int(packet[0] >> 6)
        guard kind == FaceStream.kindKey || kind == FaceStream.kindDelta else {
            throw kind == FaceStream.kindReference || kind == FaceStream.kindAtlas ? FaceStreamError.notAFrame : FaceStreamError.kind
        }
        let id = Int(packet[0] & 15)
        let frameSeq = Int(packet[1])
        let key = kind == FaceStream.kindKey
        var cfg = config
        var t: UInt32 = 0
        let body: [UInt8]
        if key {
            guard packet.count >= FaceStream.keyHeader else { throw FaceStreamError.short }
            let c = FaceStreamConfig(width: Int(packet[2]) << 8 | Int(packet[3]), height: Int(packet[4]) << 8 | Int(packet[5]), fps: Int(packet[6]), precision: Int(packet[7]), keyInterval: 1)
            guard let checked = c.normalized(strict: true) else { throw FaceStreamError.header }
            cfg = checked
            t = UInt32(packet[8]) << 24 | UInt32(packet[9]) << 16 | UInt32(packet[10]) << 8 | UInt32(packet[11])
            body = Array(packet[FaceStream.keyHeader...])
        } else {
            guard haveKey, id == keyId else { throw FaceStreamError.needKey }
            let ahead = (frameSeq - seq) & 255
            guard ahead != 0, ahead <= 128 else { throw FaceStreamError.old }
            body = Array(packet[2...])
        }
        var dec = FaceStreamRangeDecoder(body)
        let inSeq = key || (chainOK && frameSeq == (seq + 1) & 255)
        var qp: FaceStreamQPose
        if key {
            qp = FaceStreamQPose(qx: Int(dec.direct(11)) - 512, qy: Int(dec.direct(11)) - 512, qz: Int(dec.direct(11)) - 512,
                tx: Int(dec.direct(16)) - 32768, ty: Int(dec.direct(16)) - 32768, s: Int(dec.direct(15)))
            guard qp.qx <= 512, qp.qy <= 512, qp.qz <= 512, qp.s >= 1 else { throw FaceStreamError.corrupt }
        } else {
            let nominal = FaceStream.nominal(cfg.fps)
            var timeModel = FaceStreamIntModel()
            let dt = dec.int(&timeModel, 0) + nominal
            let gap = (frameSeq - seq) & 255
            t = time &+ UInt32(bitPattern: Int32(truncatingIfNeeded: (gap - 1) * nominal + dt))
            var models = faceStreamModels(6)
            let v = keyPose.values.enumerated().map { i, b in b + dec.int(&models[i], 0) }
            qp = FaceStreamQPose(qx: v[0], qy: v[1], qz: v[2], tx: v[3], ty: v[4], s: v[5])
            let quat = -512...512
            guard quat.contains(qp.qx), quat.contains(qp.qy), quat.contains(qp.qz), (-32768...32767).contains(qp.tx), (-32768...32767).contains(qp.ty),
                (1...32767).contains(qp.s)
            else { throw FaceStreamError.corrupt }
        }
        var qx: [Int] = []
        for c in FaceStream.channels {
            var model = FaceStreamIntModel(zeroPrior: c.prior)
            let value = c.signed ? dec.int(&model, 0) : dec.uint(&model, 0)
            guard value >= c.low, value <= c.high else { throw FaceStreamError.corrupt }
            qx.append(value)
        }
        let pose = qp.pose
        let now = FaceStream.project(pose)
        var frame = FaceStreamFrame(key: key, stale: false, seq: frameSeq, keyId: id, time: t, width: cfg.width, height: cfg.height, precision: cfg.precision,
            pose: pose, expression: faceStreamExpression(qx), points: [])
        if inSeq {
            var models = key ? faceStreamModels(12) : chain
            let step = pose.scale / Double(cfg.precision)
            var nonzero = [Int](repeating: 0, count: 12)
            var next: [[Double]] = Array(repeating: [0, 0], count: 68)
            for i in 0..<68 {
                for axis in 0..<2 {
                    let current = axis == 0 ? now[i].x : now[i].y
                    let before = axis == 0 ? projected[i].x : projected[i].y
                    let pred = key ? current : recon[i][axis] + (current - before)
                    let cls = FaceStream.group(i) * 2 + axis
                    let r = dec.int(&models[cls], nonzero[cls])
                    nonzero[cls] = r != 0 ? 1 : 0
                    next[i][axis] = pred + Double(r) * step
                }
            }
            guard !dec.overrun else { throw FaceStreamError.corrupt }
            chain = key ? faceStreamModels(12) : models
            recon = next
            projected = now
            lastGood = next
            goodProjected = now
            chainOK = true
            frame.points = next.map { CallFacePoint($0[0], $0[1]) }
        } else {
            guard !dec.overrun else { throw FaceStreamError.corrupt }
            chainOK = false
            frame.stale = true
            frame.points = (0..<68).map { i in
                CallFacePoint(lastGood[i][0] + (now[i].x - goodProjected[i].x), lastGood[i][1] + (now[i].y - goodProjected[i].y))
            }
        }
        if key {
            haveKey = true
            config = cfg
            keyId = id
            keyPose = qp
        }
        seq = frameSeq
        time = t
        return frame
    }
}
