import Foundation

public struct EyeVector: Equatable, Sendable {
    public var x: Double
    public var y: Double
    public var z: Double

    public init(_ x: Double, _ y: Double, _ z: Double) {
        self.x = x
        self.y = y
        self.z = z
    }

    public static let zero: EyeVector = EyeVector(0, 0, 0)

    public static func + (a: EyeVector, b: EyeVector) -> EyeVector { EyeVector(a.x + b.x, a.y + b.y, a.z + b.z) }
    public static func - (a: EyeVector, b: EyeVector) -> EyeVector { EyeVector(a.x - b.x, a.y - b.y, a.z - b.z) }
    public static func * (a: EyeVector, s: Double) -> EyeVector { EyeVector(a.x * s, a.y * s, a.z * s) }

    public var length: Double { (x * x + y * y + z * z).squareRoot() }

    public var normalized: EyeVector? {
        let size: Double = length
        return size > 1e-9 ? self * (1 / size) : nil
    }
}

public struct EyeTransform: Equatable, Sendable {
    public var columns: [Double]

    public init(columns: [Double]) {
        precondition(columns.count == 16)
        self.columns = columns
    }

    public static let identity: EyeTransform = EyeTransform(columns: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])

    public static func translation(_ v: EyeVector) -> EyeTransform {
        EyeTransform(columns: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, v.x, v.y, v.z, 1])
    }

    public static func rotationX(_ radians: Double) -> EyeTransform {
        let c: Double = cos(radians)
        let s: Double = sin(radians)
        return EyeTransform(columns: [1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1])
    }

    public static func rotationY(_ radians: Double) -> EyeTransform {
        let c: Double = cos(radians)
        let s: Double = sin(radians)
        return EyeTransform(columns: [c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1])
    }

    public var position: EyeVector { EyeVector(columns[12], columns[13], columns[14]) }

    public func point(_ v: EyeVector) -> EyeVector {
        EyeVector(
            columns[0] * v.x + columns[4] * v.y + columns[8] * v.z + columns[12],
            columns[1] * v.x + columns[5] * v.y + columns[9] * v.z + columns[13],
            columns[2] * v.x + columns[6] * v.y + columns[10] * v.z + columns[14])
    }

    public func direction(_ v: EyeVector) -> EyeVector {
        EyeVector(
            columns[0] * v.x + columns[4] * v.y + columns[8] * v.z,
            columns[1] * v.x + columns[5] * v.y + columns[9] * v.z,
            columns[2] * v.x + columns[6] * v.y + columns[10] * v.z)
    }

    public static func * (a: EyeTransform, b: EyeTransform) -> EyeTransform {
        var out: [Double] = Array(repeating: 0, count: 16)
        for column in 0..<4 {
            for row in 0..<4 {
                var sum: Double = 0
                for k in 0..<4 {
                    sum += a.columns[k * 4 + row] * b.columns[column * 4 + k]
                }
                out[column * 4 + row] = sum
            }
        }
        return EyeTransform(columns: out)
    }
}

public struct EyePoint: Equatable, Sendable, Codable {
    public var x: Double
    public var y: Double

    public init(_ x: Double, _ y: Double) {
        self.x = x
        self.y = y
    }

    public func distance(to other: EyePoint) -> Double {
        ((x - other.x) * (x - other.x) + (y - other.y) * (y - other.y)).squareRoot()
    }
}

public enum EyeCameraEdge: String, Sendable {
    case top
    case bottom
    case left
    case right
}

public struct EyeScreen: Equatable, Sendable {
    public var width: Double
    public var height: Double
    public var pointsPerMeter: Double
    public var camera: EyePoint

    public static let phonePointsPerInch: Double = 155
    public static let tabletPointsPerInch: Double = 132

    public init(width: Double, height: Double, pointsPerMeter: Double, camera: EyePoint) {
        self.width = width
        self.height = height
        self.pointsPerMeter = pointsPerMeter
        self.camera = camera
    }

    public init(width: Double, height: Double, pointsPerInch: Double, cameraEdge: EyeCameraEdge) {
        let camera: EyePoint
        switch cameraEdge {
        case .top: camera = EyePoint(width / 2, 0)
        case .bottom: camera = EyePoint(width / 2, height)
        case .left: camera = EyePoint(0, height / 2)
        case .right: camera = EyePoint(width, height / 2)
        }
        self.init(width: width, height: height, pointsPerMeter: pointsPerInch / 0.0254, camera: camera)
    }

    public func clamp(_ point: EyePoint) -> EyePoint {
        EyePoint(min(max(point.x, 0), width), min(max(point.y, 0), height))
    }
}

public enum EyeGaze {
    public static let forward: EyeVector = EyeVector(0, 0, 1)

    public static func screenPoint(face: EyeTransform, leftEye: EyeTransform, rightEye: EyeTransform, screen: EyeScreen) -> EyePoint? {
        let eyes: [EyeTransform] = [leftEye, rightEye]
        let hits: [EyeVector] = eyes.compactMap { (eye: EyeTransform) in
            hit(origin: face.point(eye.position), direction: face.direction(eye.direction(forward)))
        }
        guard hits.count == eyes.count else { return nil }
        let mean: EyeVector = hits.reduce(EyeVector.zero, +) * (1 / Double(hits.count))
        return EyePoint(screen.camera.x + mean.x * screen.pointsPerMeter, screen.camera.y - mean.y * screen.pointsPerMeter)
    }

    static func hit(origin: EyeVector, direction: EyeVector) -> EyeVector? {
        guard let direction = direction.normalized, origin.z < 0, direction.z > 1e-6 else { return nil }
        return origin + direction * (-origin.z / direction.z)
    }

    public static func distance(face: EyeTransform) -> Double {
        face.position.length
    }
}

public struct EyeCalibration: Equatable, Sendable, Codable {
    public static let curveScale: Double = 1000
    public static let curveRidge: Double = 0.05

    public var a: Double
    public var b: Double
    public var c: Double
    public var d: Double
    public var e: Double
    public var f: Double
    public var qx: [Double]?
    public var qy: [Double]?

    public init(a: Double, b: Double, c: Double, d: Double, e: Double, f: Double, qx: [Double]? = nil, qy: [Double]? = nil) {
        self.a = a
        self.b = b
        self.c = c
        self.d = d
        self.e = e
        self.f = f
        self.qx = qx
        self.qy = qy
    }

    public static let identity: EyeCalibration = EyeCalibration(a: 1, b: 0, c: 0, d: 0, e: 1, f: 0)

    public var curved: Bool { qx != nil || qy != nil }

    public func apply(_ p: EyePoint) -> EyePoint {
        var x: Double = a * p.x + b * p.y + c
        var y: Double = d * p.x + e * p.y + f
        if curved {
            let terms: [Double] = Array(Self.features(p, curved: true)[3...])
            x += zip(qx ?? [], terms).reduce(0) { $0 + $1.0 * $1.1 }
            y += zip(qy ?? [], terms).reduce(0) { $0 + $1.0 * $1.1 }
        }
        return EyePoint(x, y)
    }

    public func shifted(by offset: EyePoint) -> EyeCalibration {
        var moved: EyeCalibration = self
        moved.c += offset.x
        moved.f += offset.y
        return moved
    }

    public static func fit(raw: [EyePoint], targets: [EyePoint], weights: [Double]? = nil, curved: Bool = false) -> EyeCalibration? {
        let size: Int = curved ? 6 : 3
        guard raw.count == targets.count, raw.count >= size, (weights?.count ?? raw.count) == raw.count else { return nil }
        var m: [[Double]] = Array(repeating: Array(repeating: 0, count: size), count: size)
        var rx: [Double] = Array(repeating: 0, count: size)
        var ry: [Double] = Array(repeating: 0, count: size)
        var total: Double = 0
        for index in raw.indices {
            let weight: Double = weights?[index] ?? 1
            guard weight > 0, weight.isFinite else { continue }
            let row: [Double] = features(raw[index], curved: curved)
            for i in 0..<size {
                for j in 0..<size {
                    m[i][j] += weight * row[i] * row[j]
                }
                rx[i] += weight * row[i] * targets[index].x
                ry[i] += weight * row[i] * targets[index].y
            }
            total += weight
        }
        guard total > 0 else { return nil }
        if curved {
            for i in 3..<size {
                m[i][i] += curveRidge * total
            }
        }
        guard let xs = solve(m, rx), let ys = solve(m, ry) else { return nil }
        let s: Double = curveScale
        let fitted: EyeCalibration = EyeCalibration(
            a: xs[0] / s, b: xs[1] / s, c: xs[2], d: ys[0] / s, e: ys[1] / s, f: ys[2],
            qx: curved ? Array(xs[3...]) : nil, qy: curved ? Array(ys[3...]) : nil)
        let all: [Double] = [fitted.a, fitted.b, fitted.c, fitted.d, fitted.e, fitted.f] + (fitted.qx ?? []) + (fitted.qy ?? [])
        return all.allSatisfy(\.isFinite) ? fitted : nil
    }

    static func features(_ p: EyePoint, curved: Bool) -> [Double] {
        let u: Double = p.x / curveScale
        let v: Double = p.y / curveScale
        return curved ? [u, v, 1, u * u, u * v, v * v] : [u, v, 1]
    }

    public static func decode(_ wire: String?) -> EyeCalibration? {
        guard let data = wire?.data(using: .utf8) else { return nil }
        return try? JSONDecoder().decode(EyeCalibration.self, from: data)
    }

    public var encoded: String {
        guard let data = try? JSONEncoder().encode(self) else { return "" }
        return String(decoding: data, as: UTF8.self)
    }

    private static func solve(_ matrix: [[Double]], _ rhs: [Double]) -> [Double]? {
        var m: [[Double]] = matrix
        var v: [Double] = rhs
        let n: Int = v.count
        let scale: Double = max(matrix.flatMap { $0 }.map(abs).max() ?? 0, 1)
        for column in 0..<n {
            guard let pivot = (column..<n).max(by: { abs(m[$0][column]) < abs(m[$1][column]) }),
                abs(m[pivot][column]) > scale * 1e-12
            else { return nil }
            m.swapAt(column, pivot)
            v.swapAt(column, pivot)
            for row in 0..<n where row != column {
                let factor: Double = m[row][column] / m[column][column]
                for k in column..<n {
                    m[row][k] -= factor * m[column][k]
                }
                v[row] -= factor * v[column]
            }
        }
        return (0..<n).map { v[$0] / m[$0][$0] }
    }
}

public struct EyeOneEuro: Sendable {
    public var minCutoff: Double
    public var beta: Double
    public var derivativeCutoff: Double = 1
    private var value: Double?
    private var speed: Double = 0
    private var time: Double?

    public init(minCutoff: Double, beta: Double) {
        self.minCutoff = minCutoff
        self.beta = beta
    }

    public mutating func reset() {
        value = nil
        speed = 0
        time = nil
    }

    public mutating func filter(_ x: Double, at t: Double) -> Double {
        guard let last = value, let then = time, t > then else {
            value = x
            time = t
            return x
        }
        let dt: Double = t - then
        let rawSpeed: Double = (x - last) / dt
        speed += Self.alpha(derivativeCutoff, dt) * (rawSpeed - speed)
        let cutoff: Double = minCutoff + beta * abs(speed)
        let next: Double = last + Self.alpha(cutoff, dt) * (x - last)
        value = next
        time = t
        return next
    }

    private static func alpha(_ cutoff: Double, _ dt: Double) -> Double {
        let tau: Double = 1 / (2 * Double.pi * cutoff)
        return 1 / (1 + tau / dt)
    }
}

public enum EyeSmoothing: String, CaseIterable, Sendable {
    case steady
    case balanced
    case quick

    public static let defaultValue: EyeSmoothing = .balanced

    public static func of(_ wire: String?) -> EyeSmoothing {
        EyeSmoothing(rawValue: wire ?? "") ?? defaultValue
    }

    public var label: String {
        switch self {
        case .steady: return "Steady"
        case .balanced: return "Balanced"
        case .quick: return "Quick"
        }
    }

    public var minCutoff: Double {
        switch self {
        case .steady: return 0.3
        case .balanced: return 0.7
        case .quick: return 1.5
        }
    }

    public var beta: Double {
        switch self {
        case .steady: return 0.001
        case .balanced: return 0.003
        case .quick: return 0.008
        }
    }
}

public enum EyeBlinkLength: String, CaseIterable, Sendable {
    case quick
    case deliberate
    case long

    public static let defaultValue: EyeBlinkLength = .deliberate

    public static func of(_ wire: String?) -> EyeBlinkLength {
        EyeBlinkLength(rawValue: wire ?? "") ?? defaultValue
    }

    public var label: String {
        switch self {
        case .quick: return "Any blink"
        case .deliberate: return "Firm blink"
        case .long: return "Long blink"
        }
    }

    public var detail: String {
        switch self {
        case .quick: return "Every blink clicks, even the ones you don't notice."
        case .deliberate: return "Close both eyes for about a quarter of a second. Ordinary blinks are ignored."
        case .long: return "Close both eyes for half a second."
        }
    }

    public var minSeconds: Double {
        switch self {
        case .quick: return 0.05
        case .deliberate: return 0.25
        case .long: return 0.5
        }
    }
}

public enum EyeBlinkEvent: Equatable, Sendable {
    case closing
    case click
    case ignored
}

public struct EyeBlinkDetector: Sendable {
    public static let closingAt: Double = 0.3
    public static let closedAt: Double = 0.55
    public static let openAt: Double = 0.3
    public static let maxSeconds: Double = 1.5
    public static let cooldownSeconds: Double = 0.4

    public var length: EyeBlinkLength
    public private(set) var closingSince: Double?
    private var closedSince: Double?
    private var lastClick: Double = -.infinity

    public init(length: EyeBlinkLength = .defaultValue) {
        self.length = length
    }

    public var closing: Bool { closingSince != nil }

    public mutating func reset() {
        closingSince = nil
        closedSince = nil
    }

    public mutating func update(time: Double, left: Double, right: Double) -> EyeBlinkEvent? {
        let both: Double = min(left, right)
        let either: Double = max(left, right)
        if closingSince == nil {
            guard either >= Self.closingAt else { return nil }
            closingSince = time
            if both >= Self.closedAt { closedSince = time }
            return .closing
        }
        if closedSince == nil, both >= Self.closedAt {
            closedSince = time
        }
        guard either < Self.openAt else { return nil }
        let closedFor: Double? = closedSince.map { time - $0 }
        reset()
        guard let closedFor, closedFor >= length.minSeconds, closedFor <= Self.maxSeconds,
            time - lastClick >= Self.cooldownSeconds
        else { return .ignored }
        lastClick = time
        return .click
    }
}

public enum EyeCursorEvent: Equatable, Sendable {
    case click(EyePoint)
}

public struct EyeCursorEngine: Sendable {
    public static let historySeconds: Double = 1
    public static let leadSeconds: Double = 0.12
    public static let spreadSeconds: Double = 0.3
    public static let fixationSeconds: Double = 0.25
    public static let fixationDispersion: Double = 40
    public static let dwellSeconds: Double = 0.9
    public static let learnSeconds: Double = 0.4
    public static let learnFrames: Int = 5
    public static let learnableSize: Double = 140
    public static let roughAccuracy: Double = 60
    public static let minSigma: Double = 14
    public static let maxSigma: Double = 90
    public static let magnet: Double = 0.6

    struct Frame: Sendable {
        var time: Double
        var point: EyePoint
        var mapped: EyePoint
        var raw: EyePoint
        var target: EyeTarget?
    }

    public var calibration: EyeCalibration
    public var accuracy: Double?
    public var targets: [EyeTarget] = []
    public var snapping: Bool = true {
        didSet {
            if !snapping {
                snapper.reset()
                snapped = nil
            }
        }
    }
    public var learning: Bool = true
    public var smoothing: EyeSmoothing {
        didSet { applySmoothing() }
    }
    public var blinkLength: EyeBlinkLength {
        didSet { blink.length = blinkLength }
    }
    public private(set) var cursor: EyePoint?
    public private(set) var snapped: EyeTarget?
    public private(set) var sigma: Double = roughAccuracy
    public private(set) var fixating: Bool = false
    public private(set) var blink: EyeBlinkDetector
    private var filterX: EyeOneEuro
    private var filterY: EyeOneEuro
    private var snapper: EyeSnapper = EyeSnapper()
    private var history: [Frame] = []
    private var samples: [EyeCalibrationSample] = []
    private var fixationSince: Double?
    private var dwelled: Int?

    public init(calibration: EyeCalibration = .identity, smoothing: EyeSmoothing = .defaultValue, blinkLength: EyeBlinkLength = .defaultValue) {
        self.calibration = calibration
        self.smoothing = smoothing
        self.blinkLength = blinkLength
        blink = EyeBlinkDetector(length: blinkLength)
        filterX = EyeOneEuro(minCutoff: smoothing.minCutoff, beta: smoothing.beta)
        filterY = EyeOneEuro(minCutoff: smoothing.minCutoff, beta: smoothing.beta)
    }

    public var frozen: Bool { blink.closing }

    public var display: EyePoint? {
        guard let cursor else { return nil }
        guard let snapped else { return cursor }
        let pull: EyePoint = snapped.clickPoint(for: cursor)
        return EyePoint(cursor.x + (pull.x - cursor.x) * Self.magnet, cursor.y + (pull.y - cursor.y) * Self.magnet)
    }

    public var confidence: Double {
        guard let cursor, let snapped else { return 0 }
        let reach: Double = snapped.frame.distance(to: cursor) / sigma
        return exp(-0.5 * reach * reach)
    }

    public mutating func takeSamples() -> [EyeCalibrationSample] {
        defer { samples = [] }
        return samples
    }

    public mutating func reset() {
        cursor = nil
        snapped = nil
        fixating = false
        fixationSince = nil
        dwelled = nil
        sigma = Self.roughAccuracy
        history = []
        samples = []
        snapper.reset()
        blink.reset()
        filterX.reset()
        filterY.reset()
    }

    public mutating func update(time: Double, raw: EyePoint?, blinkLeft: Double, blinkRight: Double, screen: EyeScreen) -> EyeCursorEvent? {
        let event: EyeBlinkEvent? = blink.update(time: time, left: blinkLeft, right: blinkRight)
        if event == .click, let frame = clickFrame() {
            learn(clicked: frame)
            return .click(frame.target?.clickPoint(for: frame.point) ?? frame.point)
        }
        guard !blink.closing, let raw else { return nil }
        let mapped: EyePoint = screen.clamp(calibration.apply(raw))
        let smoothed: EyePoint = screen.clamp(EyePoint(filterX.filter(mapped.x, at: time), filterY.filter(mapped.y, at: time)))
        cursor = smoothed
        history.append(Frame(time: time, point: smoothed, mapped: mapped, raw: raw, target: nil))
        history.removeAll { time - $0.time > Self.historySeconds }
        let jitter: Double = Self.spread(history.filter { time - $0.time <= Self.spreadSeconds }.map(\.mapped))
        let accuracy: Double = self.accuracy ?? Self.roughAccuracy
        sigma = min(max((0.25 * jitter * jitter + accuracy * accuracy).squareRoot(), Self.minSigma), Self.maxSigma)
        trackFixation(time: time)
        snapped = snapping ? snapper.update(time: time, gaze: smoothed, sigma: sigma, targets: targets) : nil
        history[history.count - 1].target = snapped
        learnFromDwell(time: time)
        return nil
    }

    private mutating func trackFixation(time: Double) {
        let recent: [EyePoint] = history.filter { time - $0.time <= Self.fixationSeconds }.map(\.point)
        let covered: Bool = (history.first?.time ?? time) <= time - Self.fixationSeconds
        fixating = covered && Self.dispersion(recent) <= Self.fixationDispersion
        if fixating {
            if fixationSince == nil { fixationSince = time - Self.fixationSeconds }
        } else {
            fixationSince = nil
            dwelled = nil
        }
    }

    private mutating func learnFromDwell(time: Double) {
        guard learning, let since = fixationSince, time - since >= Self.dwellSeconds,
            let target = snapped, Self.learnable(target), dwelled != target.id,
            let raw = rawGaze(on: target, from: max(since, time - Self.learnSeconds), to: time)
        else { return }
        dwelled = target.id
        samples.append(EyeCalibrationSample(raw: raw, target: target.frame.center, source: .dwell))
    }

    private mutating func learn(clicked frame: Frame) {
        guard learning, let target = frame.target, Self.learnable(target),
            let raw = rawGaze(on: target, from: frame.time - Self.learnSeconds, to: frame.time)
        else { return }
        samples.append(EyeCalibrationSample(raw: raw, target: target.frame.center, source: .click))
    }

    private func rawGaze(on target: EyeTarget, from start: Double, to end: Double) -> EyePoint? {
        let frames: [Frame] = history.filter { $0.time >= start && $0.time <= end && $0.target?.id == target.id }
        guard frames.count >= Self.learnFrames, Self.dispersion(frames.map(\.point)) <= Self.fixationDispersion * 1.5 else { return nil }
        return EyeStats.median(frames.map(\.raw))
    }

    static func learnable(_ target: EyeTarget) -> Bool {
        !target.precise && target.frame.width <= learnableSize && target.frame.height <= learnableSize
    }

    static func spread(_ points: [EyePoint]) -> Double {
        guard points.count > 1 else { return 0 }
        let n: Double = Double(points.count)
        let mx: Double = points.reduce(0) { $0 + $1.x } / n
        let my: Double = points.reduce(0) { $0 + $1.y } / n
        let squares: Double = points.reduce(0) { $0 + ($1.x - mx) * ($1.x - mx) + ($1.y - my) * ($1.y - my) }
        return (squares / n).squareRoot()
    }

    static func dispersion(_ points: [EyePoint]) -> Double {
        guard let first = points.first else { return 0 }
        var low: EyePoint = first
        var high: EyePoint = first
        for point in points {
            low = EyePoint(min(low.x, point.x), min(low.y, point.y))
            high = EyePoint(max(high.x, point.x), max(high.y, point.y))
        }
        return (high.x - low.x) + (high.y - low.y)
    }

    private func clickFrame() -> Frame? {
        guard let last = history.last else { return nil }
        let before: Double = last.time - Self.leadSeconds
        return history.last { $0.time <= before } ?? history.first ?? last
    }

    private mutating func applySmoothing() {
        filterX = EyeOneEuro(minCutoff: smoothing.minCutoff, beta: smoothing.beta)
        filterY = EyeOneEuro(minCutoff: smoothing.minCutoff, beta: smoothing.beta)
    }
}

public struct EyeCalibrationRun: Sendable {
    public static let settleSeconds: Double = 0.8
    public static let collectSeconds: Double = 1.0
    public static let maxCollectSeconds: Double = 2.0
    public static let wantedSamples: Int = 20
    public static let outlierSpread: Double = 2.5
    public static let minimumTargets: Int = 5

    public let targets: [EyePoint]
    public private(set) var index: Int = 0
    public private(set) var finished: Bool = false
    public private(set) var dotProgress: Double = 0
    public private(set) var samples: [EyeCalibrationSample] = []
    private var startedAt: Double?
    private var gathered: [EyePoint] = []

    public init(screen: EyeScreen, inset: Double = 0.12) {
        let xs: [Double] = [inset, 0.5, 1 - inset].map { $0 * screen.width }
        let ys: [Double] = [inset, 0.5, 1 - inset].map { $0 * screen.height }
        targets = ys.flatMap { (y: Double) in xs.map { EyePoint($0, y) } }
    }

    public var target: EyePoint? { finished ? nil : targets[index] }

    public var progress: Double { Double(index) / Double(targets.count) }

    public mutating func update(time: Double, raw point: EyePoint?) {
        guard !finished else { return }
        let start: Double = startedAt ?? time
        startedAt = start
        let elapsed: Double = time - start
        if elapsed >= Self.settleSeconds, let point {
            gathered.append(point)
        }
        dotProgress = min(max((elapsed - Self.settleSeconds) / Self.collectSeconds, 0), 1)
        let enough: Bool = gathered.count >= Self.wantedSamples || elapsed >= Self.settleSeconds + Self.maxCollectSeconds
        guard elapsed >= Self.settleSeconds + Self.collectSeconds, enough else { return }
        if let center = Self.robustCenter(gathered) {
            samples.append(EyeCalibrationSample(raw: center, target: targets[index], source: .dot))
        }
        gathered = []
        startedAt = nil
        dotProgress = 0
        index += 1
        finished = index >= targets.count
    }

    public var live: EyeLiveCalibration? {
        guard finished, samples.count >= Self.minimumTargets else { return nil }
        let fitted: EyeLiveCalibration = EyeLiveCalibration(dots: samples)
        return fitted.model == .rough ? nil : fitted
    }

    public var calibration: EyeCalibration? { live?.calibration }

    static func median(_ points: [EyePoint]) -> EyePoint? {
        EyeStats.median(points)
    }

    static func robustCenter(_ points: [EyePoint]) -> EyePoint? {
        guard let middle = EyeStats.median(points) else { return nil }
        let distances: [Double] = points.map { $0.distance(to: middle) }
        let mad: Double = EyeStats.median(distances) ?? 0
        let limit: Double = max(outlierSpread * 1.4826 * mad, 4)
        let kept: [EyePoint] = points.filter { $0.distance(to: middle) <= limit }
        guard !kept.isEmpty else { return middle }
        let n: Double = Double(kept.count)
        return EyePoint(kept.reduce(0) { $0 + $1.x } / n, kept.reduce(0) { $0 + $1.y } / n)
    }
}

enum EyeStats {
    static func median(_ values: [Double]) -> Double? {
        guard !values.isEmpty else { return nil }
        let sorted: [Double] = values.sorted()
        let middle: Int = sorted.count / 2
        return sorted.count % 2 == 1 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2
    }

    static func median(_ points: [EyePoint]) -> EyePoint? {
        guard let x = median(points.map(\.x)), let y = median(points.map(\.y)) else { return nil }
        return EyePoint(x, y)
    }
}

public enum EyeControlPrefs {
    public static let smoothing: String = "eyeControl.smoothing"
    public static let blinkLength: String = "eyeControl.blinkLength"
    public static let calibration: String = "eyeControl.calibration"
    public static let snapping: String = "eyeControl.snapping"
    public static let learning: String = "eyeControl.learning"
}
