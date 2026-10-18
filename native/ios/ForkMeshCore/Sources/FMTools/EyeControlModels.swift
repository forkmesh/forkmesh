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
        let origin: EyeVector = (face.point(leftEye.position) + face.point(rightEye.position)) * 0.5
        let summed: EyeVector = face.direction(leftEye.direction(forward)) + face.direction(rightEye.direction(forward))
        guard let direction = summed.normalized, origin.z < 0, direction.z > 1e-6 else { return nil }
        let hit: EyeVector = origin + direction * (-origin.z / direction.z)
        return EyePoint(screen.camera.x + hit.x * screen.pointsPerMeter, screen.camera.y - hit.y * screen.pointsPerMeter)
    }

    public static func distance(face: EyeTransform) -> Double {
        face.position.length
    }
}

public struct EyeCalibration: Equatable, Sendable, Codable {
    public var a: Double
    public var b: Double
    public var c: Double
    public var d: Double
    public var e: Double
    public var f: Double

    public init(a: Double, b: Double, c: Double, d: Double, e: Double, f: Double) {
        self.a = a
        self.b = b
        self.c = c
        self.d = d
        self.e = e
        self.f = f
    }

    public static let identity: EyeCalibration = EyeCalibration(a: 1, b: 0, c: 0, d: 0, e: 1, f: 0)

    public func apply(_ p: EyePoint) -> EyePoint {
        EyePoint(a * p.x + b * p.y + c, d * p.x + e * p.y + f)
    }

    public static func fit(raw: [EyePoint], targets: [EyePoint]) -> EyeCalibration? {
        guard raw.count == targets.count, raw.count >= 3 else { return nil }
        var m: [[Double]] = Array(repeating: Array(repeating: 0, count: 3), count: 3)
        var rx: [Double] = [0, 0, 0]
        var ry: [Double] = [0, 0, 0]
        for (point, target) in zip(raw, targets) {
            let row: [Double] = [point.x, point.y, 1]
            for i in 0..<3 {
                for j in 0..<3 {
                    m[i][j] += row[i] * row[j]
                }
                rx[i] += row[i] * target.x
                ry[i] += row[i] * target.y
            }
        }
        guard let xs = solve(m, rx), let ys = solve(m, ry) else { return nil }
        let fitted: EyeCalibration = EyeCalibration(a: xs[0], b: xs[1], c: xs[2], d: ys[0], e: ys[1], f: ys[2])
        return [fitted.a, fitted.b, fitted.c, fitted.d, fitted.e, fitted.f].allSatisfy(\.isFinite) ? fitted : nil
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

    public var calibration: EyeCalibration
    public var smoothing: EyeSmoothing {
        didSet { applySmoothing() }
    }
    public var blinkLength: EyeBlinkLength {
        didSet { blink.length = blinkLength }
    }
    public private(set) var cursor: EyePoint?
    public private(set) var blink: EyeBlinkDetector
    private var filterX: EyeOneEuro
    private var filterY: EyeOneEuro
    private var history: [(time: Double, point: EyePoint)] = []

    public init(calibration: EyeCalibration = .identity, smoothing: EyeSmoothing = .defaultValue, blinkLength: EyeBlinkLength = .defaultValue) {
        self.calibration = calibration
        self.smoothing = smoothing
        self.blinkLength = blinkLength
        blink = EyeBlinkDetector(length: blinkLength)
        filterX = EyeOneEuro(minCutoff: smoothing.minCutoff, beta: smoothing.beta)
        filterY = EyeOneEuro(minCutoff: smoothing.minCutoff, beta: smoothing.beta)
    }

    public var frozen: Bool { blink.closing }

    public mutating func reset() {
        cursor = nil
        history = []
        blink.reset()
        filterX.reset()
        filterY.reset()
    }

    public mutating func update(time: Double, raw: EyePoint?, blinkLeft: Double, blinkRight: Double, screen: EyeScreen) -> EyeCursorEvent? {
        let event: EyeBlinkEvent? = blink.update(time: time, left: blinkLeft, right: blinkRight)
        if event == .click, let target = clickTarget() {
            return .click(target)
        }
        guard !blink.closing, let raw else { return nil }
        let mapped: EyePoint = screen.clamp(calibration.apply(raw))
        let smoothed: EyePoint = screen.clamp(EyePoint(filterX.filter(mapped.x, at: time), filterY.filter(mapped.y, at: time)))
        cursor = smoothed
        history.append((time, smoothed))
        history.removeAll { time - $0.time > Self.historySeconds }
        return nil
    }

    private func clickTarget() -> EyePoint? {
        guard let last = history.last else { return cursor }
        let before: Double = last.time - Self.leadSeconds
        return history.last { $0.time <= before }?.point ?? history.first?.point ?? last.point
    }

    private mutating func applySmoothing() {
        filterX = EyeOneEuro(minCutoff: smoothing.minCutoff, beta: smoothing.beta)
        filterY = EyeOneEuro(minCutoff: smoothing.minCutoff, beta: smoothing.beta)
    }
}

public struct EyeCalibrationRun: Sendable {
    public static let settleSeconds: Double = 0.8
    public static let collectSeconds: Double = 1.0
    public static let minimumTargets: Int = 5

    public let targets: [EyePoint]
    public private(set) var index: Int = 0
    public private(set) var finished: Bool = false
    private var startedAt: Double?
    private var samples: [EyePoint] = []
    private var raw: [EyePoint] = []
    private var hits: [EyePoint] = []

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
            samples.append(point)
        }
        guard elapsed >= Self.settleSeconds + Self.collectSeconds else { return }
        if let median = Self.median(samples) {
            raw.append(median)
            hits.append(targets[index])
        }
        samples = []
        startedAt = nil
        index += 1
        finished = index >= targets.count
    }

    public var calibration: EyeCalibration? {
        guard finished, raw.count >= Self.minimumTargets else { return nil }
        return EyeCalibration.fit(raw: raw, targets: hits)
    }

    static func median(_ points: [EyePoint]) -> EyePoint? {
        guard !points.isEmpty else { return nil }
        let xs: [Double] = points.map(\.x).sorted()
        let ys: [Double] = points.map(\.y).sorted()
        return EyePoint(xs[xs.count / 2], ys[ys.count / 2])
    }
}

public enum EyeControlPrefs {
    public static let smoothing: String = "eyeControl.smoothing"
    public static let blinkLength: String = "eyeControl.blinkLength"
    public static let calibration: String = "eyeControl.calibration"
}
