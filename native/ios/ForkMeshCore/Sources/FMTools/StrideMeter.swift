import Foundation

public enum StrideGait: String, Sendable {
    case still
    case walking
    case running
}

public struct MotionSample: Equatable, Sendable {
    public let x: Double
    public let y: Double
    public let z: Double
    public let time: TimeInterval

    public init(x: Double, y: Double, z: Double, time: TimeInterval) {
        self.x = x
        self.y = y
        self.z = z
        self.time = time
    }
}

public struct StrideReading: Equatable, Sendable {
    public var cadence: Double
    public var gait: StrideGait
    public var impact: Double

    public init(cadence: Double = 0, gait: StrideGait = .still, impact: Double = 0) {
        self.cadence = cadence
        self.gait = gait
        self.impact = impact
    }
}

public struct StrideMeter: Sendable {
    public static let sampleInterval: TimeInterval = 1.0 / 50
    public static let minPeak: Double = 0.08
    public static let minStepGap: TimeInterval = 0.25
    public static let maxStepGap: TimeInterval = 2
    public static let window: TimeInterval = 4
    public static let recentSteps: Int = 6
    public static let runEnterCadence: Double = 2.2
    public static let runEnterImpact: Double = 1.0
    public static let runFastCadence: Double = 2.6
    public static let runFastImpact: Double = 0.6
    public static let runLeaveCadence: Double = 2.0
    public static let runLeaveImpact: Double = 0.75

    private var gravity: Double?
    private var smooth: Double = 0
    private var previous: Double = 0
    private var previousTime: TimeInterval = 0
    private var rising: Bool = false
    private var armed: Bool = true
    private var peakAverage: Double = 0
    private var steps: [TimeInterval] = []
    private var lastTime: TimeInterval?
    public private(set) var reading: StrideReading = StrideReading()

    public init() {}

    public mutating func reset() { self = StrideMeter() }

    public var threshold: Double { max(Self.minPeak, peakAverage * 0.45) }

    @discardableResult
    public mutating func add(_ sample: MotionSample) -> StrideReading {
        if let lastTime, sample.time <= lastTime { return reading }
        let dt: Double = lastTime.map { min(0.5, sample.time - $0) } ?? Self.sampleInterval
        lastTime = sample.time
        let magnitude: Double = (sample.x * sample.x + sample.y * sample.y + sample.z * sample.z).squareRoot()
        let base: Double = gravity ?? magnitude
        let gravityNow: Double = base + (magnitude - base) * min(1, dt / 1.0)
        gravity = gravityNow
        let dynamic: Double = magnitude - gravityNow
        let value: Double = smooth + (dynamic - smooth) * min(1, dt / 0.04)
        smooth = value
        if value < 0 { armed = true }
        if rising && value < previous && armed && previous >= threshold {
            step(at: previousTime, height: previous)
        }
        rising = value > previous
        previous = value
        previousTime = sample.time
        settle(at: sample.time)
        return reading
    }

    private mutating func step(at time: TimeInterval, height: Double) {
        if let last = steps.last, time - last < Self.minStepGap { return }
        armed = false
        if let last = steps.last, time - last > Self.maxStepGap { steps.removeAll() }
        steps.append(time)
        steps.removeAll { time - $0 > Self.window }
        if steps.count > Self.recentSteps { steps.removeFirst(steps.count - Self.recentSteps) }
        peakAverage = peakAverage == 0 ? height : peakAverage + (height - peakAverage) * 0.25
    }

    private mutating func settle(at time: TimeInterval) {
        guard let last = steps.last, steps.count >= 3 else {
            if let last = steps.last, time - last > Self.maxStepGap { steps.removeAll() }
            reading = StrideReading(impact: peakAverage)
            return
        }
        let span: Double = last - steps[0]
        let cadence: Double = span > 0 ? Double(steps.count - 1) / span : 0
        let gap: Double = cadence > 0 ? 1 / cadence : Self.maxStepGap
        if time - last > min(Self.maxStepGap, max(1, gap * 2.5)) {
            steps.removeAll()
            peakAverage *= 0.5
            reading = StrideReading(impact: peakAverage)
            return
        }
        reading = StrideReading(cadence: min(4, cadence), gait: gait(cadence: cadence), impact: peakAverage)
    }

    private func gait(cadence: Double) -> StrideGait {
        if reading.gait == .running {
            return cadence >= Self.runLeaveCadence && peakAverage >= Self.runLeaveImpact ? .running : .walking
        }
        let hard: Bool = cadence >= Self.runEnterCadence && peakAverage >= Self.runEnterImpact
        let fast: Bool = cadence >= Self.runFastCadence && peakAverage >= Self.runFastImpact
        return hard || fast ? .running : .walking
    }
}
