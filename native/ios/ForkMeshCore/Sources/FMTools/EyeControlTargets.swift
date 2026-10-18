import Foundation

public struct EyeRect: Equatable, Sendable {
    public var x: Double
    public var y: Double
    public var width: Double
    public var height: Double

    public init(x: Double, y: Double, width: Double, height: Double) {
        self.x = x
        self.y = y
        self.width = width
        self.height = height
    }

    public var center: EyePoint { EyePoint(x + width / 2, y + height / 2) }

    public var area: Double { width * height }

    public func contains(_ p: EyePoint) -> Bool {
        p.x >= x && p.x <= x + width && p.y >= y && p.y <= y + height
    }

    public func distance(to p: EyePoint) -> Double {
        let dx: Double = max(x - p.x, 0, p.x - (x + width))
        let dy: Double = max(y - p.y, 0, p.y - (y + height))
        return (dx * dx + dy * dy).squareRoot()
    }

    public func clamp(_ p: EyePoint, inset: Double) -> EyePoint {
        let ix: Double = min(inset, width / 2)
        let iy: Double = min(inset, height / 2)
        return EyePoint(min(max(p.x, x + ix), x + width - ix), min(max(p.y, y + iy), y + height - iy))
    }

    public func overlap(_ other: EyeRect) -> Double {
        let w: Double = min(x + width, other.x + other.width) - max(x, other.x)
        let h: Double = min(y + height, other.y + other.height) - max(y, other.y)
        guard w > 0, h > 0 else { return 0 }
        let shared: Double = w * h
        return shared / (area + other.area - shared)
    }
}

public struct EyeTarget: Equatable, Sendable {
    public static let preciseInset: Double = 6

    public var id: Int
    public var frame: EyeRect
    public var precise: Bool

    public init(id: Int, frame: EyeRect, precise: Bool = false) {
        self.id = id
        self.frame = frame
        self.precise = precise
    }

    public func clickPoint(for gaze: EyePoint) -> EyePoint {
        precise ? frame.clamp(gaze, inset: Self.preciseInset) : frame.center
    }
}

public struct EyeSnapper: Sendable {
    public static let reach: Double = 2.5
    public static let release: Double = 3.5
    public static let maxReach: Double = 110
    public static let stickiness: Double = 0.6
    public static let switchSeconds: Double = 0.12
    public static let sizeCost: Double = 0.35
    public static let sameTarget: Double = 0.6

    public private(set) var current: EyeTarget?
    private var pendingID: Int?
    private var pendingSince: Double = 0

    public init() {}

    public mutating func reset() {
        current = nil
        pendingID = nil
    }

    public static func cost(_ target: EyeTarget, gaze: EyePoint, sigma: Double) -> Double {
        target.frame.distance(to: gaze) / sigma + sizeCost * log(1 + target.frame.area / (4 * sigma * sigma))
    }

    public mutating func update(time: Double, gaze: EyePoint, sigma: Double, targets: [EyeTarget]) -> EyeTarget? {
        let reachable: Double = min(Self.reach * sigma, Self.maxReach)
        let best: EyeTarget? = targets
            .filter { $0.frame.distance(to: gaze) <= reachable }
            .min { Self.cost($0, gaze: gaze, sigma: sigma) < Self.cost($1, gaze: gaze, sigma: sigma) }
        if let held = current.flatMap({ Self.find($0, in: targets) }),
            held.frame.distance(to: gaze) <= min(Self.release * sigma, Self.maxReach * 1.4)
        {
            current = held
            guard let best, best.id != held.id,
                Self.cost(best, gaze: gaze, sigma: sigma) < Self.cost(held, gaze: gaze, sigma: sigma) - Self.stickiness
            else {
                pendingID = nil
                return held
            }
            if pendingID != best.id {
                pendingID = best.id
                pendingSince = time
            } else if time - pendingSince >= Self.switchSeconds {
                current = best
                pendingID = nil
            }
            return current
        }
        pendingID = nil
        current = best
        return best
    }

    static func find(_ target: EyeTarget, in targets: [EyeTarget]) -> EyeTarget? {
        if let same = targets.first(where: { $0.id == target.id }) { return same }
        let moved: EyeTarget? = targets.max { $0.frame.overlap(target.frame) < $1.frame.overlap(target.frame) }
        return moved.flatMap { $0.frame.overlap(target.frame) >= sameTarget ? $0 : nil }
    }
}

public struct EyeCalibrationSample: Equatable, Sendable, Codable {
    public enum Source: String, Sendable, Codable {
        case dot
        case click
        case dwell
    }

    public var raw: EyePoint
    public var target: EyePoint
    public var source: Source

    public init(raw: EyePoint, target: EyePoint, source: Source) {
        self.raw = raw
        self.target = target
        self.source = source
    }

    public var weight: Double {
        switch source {
        case .dot: return 1
        case .click: return 1.5
        case .dwell: return 0.4
        }
    }
}

public enum EyeCalibrationModel: String, Sendable, Codable {
    case rough
    case offset
    case affine
    case curved
}

public struct EyeLiveCalibration: Equatable, Sendable, Codable {
    public static let maxLive: Int = 48
    public static let fadeRank: Double = 24
    public static let driftSamples: Int = 8
    public static let driftFadeRank: Double = 3
    public static let driftShrink: Double = 1
    public static let curvedSamples: Int = 8
    public static let curvedGain: Double = 0.9
    public static let huber: Double = 1.5
    public static let huberFloor: Double = 4
    public static let robustPasses: Int = 2
    public static let gateFloor: Double = 100
    public static let gateRough: Double = 240

    public private(set) var prior: EyeCalibration
    public private(set) var dots: [EyeCalibrationSample]
    public private(set) var live: [EyeCalibrationSample]
    public private(set) var calibration: EyeCalibration
    public private(set) var model: EyeCalibrationModel
    public private(set) var error: Double?
    public private(set) var drift: EyePoint

    public init(prior: EyeCalibration = .identity, dots: [EyeCalibrationSample] = [], live: [EyeCalibrationSample] = []) {
        self.prior = prior
        self.dots = dots
        self.live = Array(live.suffix(Self.maxLive))
        calibration = prior
        model = .rough
        error = nil
        drift = EyePoint(0, 0)
        refit()
    }

    public var calibrated: Bool { dots.count >= EyeCalibrationRun.minimumTargets || prior != .identity }

    public var gate: Double { error.map { max(Self.gateFloor, 3 * $0) } ?? Self.gateRough }

    @discardableResult
    public mutating func learn(_ sample: EyeCalibrationSample) -> Bool {
        guard calibration.apply(sample.raw).distance(to: sample.target) <= gate else { return false }
        live.append(sample)
        if live.count > Self.maxLive { live.removeFirst(live.count - Self.maxLive) }
        refit()
        return true
    }

    public static func decode(_ wire: String?) -> EyeLiveCalibration? {
        guard let data = wire?.data(using: .utf8) else { return nil }
        if let saved = try? JSONDecoder().decode(EyeLiveCalibration.self, from: data) { return saved }
        return EyeCalibration.decode(wire).map { EyeLiveCalibration(prior: $0) }
    }

    public var encoded: String {
        guard let data = try? JSONEncoder().encode(self) else { return "" }
        return String(decoding: data, as: UTF8.self)
    }

    var weighted: [(sample: EyeCalibrationSample, weight: Double)] {
        let fresh: [(sample: EyeCalibrationSample, weight: Double)] = live.enumerated().map { (index: Int, sample: EyeCalibrationSample) in
            (sample, sample.weight * pow(0.5, Double(live.count - 1 - index) / Self.fadeRank))
        }
        return dots.map { ($0, $0.weight) } + fresh
    }

    mutating func refit() {
        let samples: [(sample: EyeCalibrationSample, weight: Double)] = weighted
        var global: EyeCalibration = prior
        var chosen: EyeCalibrationModel = .rough
        var estimate: Double?
        if let affine = Self.robust(samples, curved: false), let affineError = affine.error,
            dots.count >= EyeCalibrationRun.minimumTargets || affineError < Self.offsetError(samples, under: prior) ?? .infinity
        {
            global = affine.calibration
            chosen = .affine
            estimate = affineError
            if samples.count >= Self.curvedSamples,
                let curved = Self.robust(samples, curved: true), let curvedError = curved.error,
                curvedError < affineError * Self.curvedGain
            {
                global = curved.calibration
                chosen = .curved
                estimate = curvedError
            }
        }
        drift = Self.drift(live, under: global)
        if chosen == .rough, !live.isEmpty { chosen = .offset }
        calibration = global.shifted(by: drift)
        model = chosen
        error = estimate
    }

    static func robust(_ samples: [(sample: EyeCalibrationSample, weight: Double)], curved: Bool) -> (calibration: EyeCalibration, error: Double?)? {
        let raw: [EyePoint] = samples.map(\.sample.raw)
        let targets: [EyePoint] = samples.map(\.sample.target)
        var weights: [Double] = samples.map(\.weight)
        guard var fitted = EyeCalibration.fit(raw: raw, targets: targets, weights: weights, curved: curved) else { return nil }
        for _ in 0..<robustPasses {
            let residuals: [Double] = raw.indices.map { fitted.apply(raw[$0]).distance(to: targets[$0]) }
            let limit: Double = max(huber * (EyeStats.median(residuals) ?? 0), huberFloor)
            let next: [Double] = raw.indices.map { samples[$0].weight * min(1, limit / max(residuals[$0], 1e-9)) }
            guard let refitted = EyeCalibration.fit(raw: raw, targets: targets, weights: next, curved: curved) else { break }
            fitted = refitted
            weights = next
        }
        return (fitted, leaveOneOut(raw: raw, targets: targets, weights: weights, curved: curved))
    }

    static func offsetError(_ samples: [(sample: EyeCalibrationSample, weight: Double)], under calibration: EyeCalibration) -> Double? {
        guard samples.count >= 2 else { return nil }
        let total: Double = samples.reduce(0) { $0 + $1.weight }
        guard total > 0 else { return nil }
        let misses: [(dx: Double, dy: Double, weight: Double)] = samples.map { (sample: EyeCalibrationSample, weight: Double) in
            let mapped: EyePoint = calibration.apply(sample.raw)
            return (sample.target.x - mapped.x, sample.target.y - mapped.y, weight)
        }
        let mx: Double = misses.reduce(0) { $0 + $1.weight * $1.dx } / total
        let my: Double = misses.reduce(0) { $0 + $1.weight * $1.dy } / total
        let squares: Double = misses.reduce(0) { $0 + $1.weight * (($1.dx - mx) * ($1.dx - mx) + ($1.dy - my) * ($1.dy - my)) }
        let n: Double = Double(samples.count)
        return (squares / total * n / (n - 1)).squareRoot()
    }

    static func leaveOneOut(raw: [EyePoint], targets: [EyePoint], weights: [Double], curved: Bool) -> Double? {
        guard raw.count >= (curved ? 7 : 4) else { return nil }
        var squares: Double = 0
        var total: Double = 0
        for index in raw.indices where weights[index] > 0 {
            var without: [Double] = weights
            without[index] = 0
            guard let held = EyeCalibration.fit(raw: raw, targets: targets, weights: without, curved: curved) else { return nil }
            let miss: Double = held.apply(raw[index]).distance(to: targets[index])
            squares += weights[index] * miss * miss
            total += weights[index]
        }
        return total > 0 ? (squares / total).squareRoot() : nil
    }

    static func drift(_ live: [EyeCalibrationSample], under calibration: EyeCalibration) -> EyePoint {
        var sx: Double = 0
        var sy: Double = 0
        var total: Double = 0
        for (rank, sample) in live.suffix(driftSamples).reversed().enumerated() {
            let weight: Double = sample.weight * pow(0.5, Double(rank) / driftFadeRank)
            let mapped: EyePoint = calibration.apply(sample.raw)
            sx += weight * (sample.target.x - mapped.x)
            sy += weight * (sample.target.y - mapped.y)
            total += weight
        }
        guard total > 0 else { return EyePoint(0, 0) }
        return EyePoint(sx / (total + driftShrink), sy / (total + driftShrink))
    }
}
