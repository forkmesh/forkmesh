import Foundation

public enum WalkMotion: String, Equatable, Sendable {
    case walking
    case running
    case still
    case unknown

    public var moving: Bool { self == .walking || self == .running }
}

public struct WalkMotionSample: Equatable, Sendable {
    public var motion: WalkMotion
    public var confident: Bool
    public var date: Date

    public init(motion: WalkMotion, confident: Bool, date: Date) {
        self.motion = motion
        self.confident = confident
        self.date = date
    }
}

public enum WalkEvent: Equatable, Sendable {
    case started(Date)
    case ended(Date)
}

public struct WalkDetector: Equatable, Sendable {
    public static let startAfter: TimeInterval = 30
    public static let endAfter: TimeInterval = 3 * 60

    public private(set) var walkStartedAt: Date?
    public private(set) var movingSince: Date?
    public private(set) var stillSince: Date?

    public init() {}

    public var walking: Bool { walkStartedAt != nil }

    public var nextCheck: Date? {
        if walking { return stillSince?.addingTimeInterval(Self.endAfter) }
        return movingSince?.addingTimeInterval(Self.startAfter)
    }

    public mutating func observe(_ sample: WalkMotionSample, now: Date) -> WalkEvent? {
        switch sample.motion {
        case .unknown:
            break
        case .walking, .running:
            if sample.confident {
                stillSince = nil
                if !walking && movingSince == nil { movingSince = sample.date }
            }
        case .still:
            if let started = walkStartedAt {
                if stillSince == nil { stillSince = max(sample.date, started) }
            } else {
                movingSince = nil
            }
        }
        return tick(now)
    }

    public mutating func forgetPendingStart() {
        movingSince = nil
    }

    public mutating func tick(_ now: Date) -> WalkEvent? {
        if walking {
            guard let still = stillSince, now.timeIntervalSince(still) >= Self.endAfter else { return nil }
            self = WalkDetector()
            return .ended(still)
        }
        guard let moving = movingSince, now.timeIntervalSince(moving) >= Self.startAfter else { return nil }
        walkStartedAt = moving
        movingSince = nil
        stillSince = nil
        return .started(moving)
    }

    public mutating func replay(_ samples: [WalkMotionSample], until now: Date) -> WalkEvent? {
        for sample in samples.sorted(by: { $0.date < $1.date }) {
            if let event = tick(sample.date) ?? observe(sample, now: sample.date) { return event }
        }
        return tick(now)
    }
}

public struct WalkProgress: Equatable, Sendable {
    public let startedAt: Date
    public var baseline: Int
    public var steps: Int
    public var distanceM: Double

    public init(startedAt: Date, baseline: Int = 0, steps: Int = 0, distanceM: Double = 0) {
        self.startedAt = startedAt
        self.baseline = baseline
        self.steps = steps
        self.distanceM = distanceM
    }

    public var todaySteps: Int { baseline + steps }

    public mutating func record(steps: Int, distanceM: Double) {
        self.steps = max(self.steps, steps)
        self.distanceM = max(self.distanceM, distanceM)
    }
}

public struct WalkPace: Equatable, Sendable {
    public static let staleAfter: TimeInterval = 10
    public static let minCadence: Double = 0.5
    public static let maxCadence: Double = 4

    public private(set) var cadence: Double = 0
    public private(set) var updatedAt: Date?
    private var lastSteps: Int?
    private var lastAt: Date?

    public init() {}

    public var moving: Bool { cadence > 0 }
    public var stepsPerMinute: Int { Int((cadence * 60).rounded()) }
    public var running: Bool { StepsText.gait(stepsPerMinute) == "Running" }
    public var expiresAt: Date? { moving ? updatedAt?.addingTimeInterval(Self.staleAfter) : nil }

    public mutating func record(steps: Int, cadence reported: Double, at now: Date) {
        var rate: Double = reported
        if rate <= 0, let lastSteps, let lastAt, steps > lastSteps {
            let elapsed: TimeInterval = now.timeIntervalSince(lastAt)
            if elapsed >= 1 && elapsed <= Self.staleAfter * 2 { rate = Double(steps - lastSteps) / elapsed }
        }
        lastSteps = steps
        lastAt = now
        cadence = rate >= Self.minCadence ? min(rate, Self.maxCadence) : 0
        updatedAt = now
    }

    @discardableResult
    public mutating func expire(_ now: Date) -> Bool {
        guard let expiresAt, now >= expiresAt else { return false }
        cadence = 0
        return true
    }
}
