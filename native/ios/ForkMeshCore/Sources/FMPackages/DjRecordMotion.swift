import Foundation

public struct DjRecordMotion: Equatable, Sendable {
    public enum Action: Equatable, Sendable {
        case none
        case turn(Double)
        case spin(from: Double, speed: Double)
    }

    public static let settled = 0.0005
    public static let slip = 0.01
    public static let still = 0.0001

    private var shown: Double?
    private var free: Double?
    private var freeAngle: Double = 0
    private var freeSince: Double = 0

    public init() {}

    public var spinning: Bool { free != nil }

    public static func steadySpeed(playing: Bool, scratching: Bool, drive: Double, motor: Double) -> Double? {
        guard playing, !scratching, drive > 0.05, abs(motor - drive) < settled else { return nil }
        return motor * 2 * Double.pi / DjStudio.secondsPerTurn
    }

    public mutating func step(angle: Double, at now: Double, steady speed: Double?) -> Action {
        guard angle.isFinite, now.isFinite else { return .none }
        if let speed, speed.isFinite, speed > 0 {
            if let free, abs(Self.wrapped(angle - freeAngle - free * (now - freeSince))) < Self.slip { return .none }
            free = speed
            freeAngle = angle
            freeSince = now
            shown = nil
            return .spin(from: angle, speed: speed)
        }
        if free == nil, let shown, abs(Self.wrapped(angle - shown)) < Self.still { return .none }
        free = nil
        shown = angle
        return .turn(angle)
    }

    public mutating func forget() {
        shown = nil
        free = nil
    }

    public static func wrapped(_ radians: Double) -> Double {
        let turn = 2 * Double.pi
        var value = radians.truncatingRemainder(dividingBy: turn)
        if value > Double.pi { value -= turn } else if value < -Double.pi { value += turn }
        return value
    }
}
