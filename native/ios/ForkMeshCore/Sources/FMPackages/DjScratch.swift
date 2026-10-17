import Foundation

public final class DjScratchAudio: Sendable {
    public static let longest: Double = 15 * 60

    public let samples: [Int16]
    public let rate: Double

    public init(samples: [Int16], rate: Double) {
        self.samples = samples
        self.rate = rate
    }

    public var duration: Double { rate > 0 ? Double(samples.count) / rate : 0 }

    public static func fits(seconds: Double) -> Bool {
        seconds.isFinite && seconds > 0 && seconds <= longest
    }

    public static func pcm(_ value: Float) -> Int16 {
        guard value.isFinite else { return 0 }
        return Int16((min(1, max(-1, value)) * 32767).rounded())
    }
}

public struct DjScratchHead: Sendable {
    public static let follow: Double = 2 * Double.pi * 22
    public static let fullSpeed: Double = 0.15
    public static let fastest: Double = 10

    public private(set) var position: Double
    public private(set) var velocity: Double = 0
    public private(set) var level: Float = 0
    private var lastIn: Float = 0
    private var lastOut: Float = 0

    public init(position: Double = 0) {
        self.position = position
    }

    public var audible: Bool { level > 0.0005 }

    public mutating func place(_ seconds: Double) {
        position = seconds.isFinite ? max(0, seconds) : 0
        velocity = 0
        level = 0
        lastIn = 0
        lastOut = 0
    }

    public mutating func render(
        _ pcm: UnsafeBufferPointer<Int16>, sourceRate: Double, outputRate: Double, goal: Double, gain: Float,
        into out: UnsafeMutableBufferPointer<Float>
    ) {
        let count = pcm.count
        guard count > 1, sourceRate > 0, outputRate > 0 else {
            for index in out.indices { out[index] = 0 }
            level = 0
            return
        }
        let end = Double(count - 1) / sourceRate
        let target = goal.isFinite ? min(end, max(0, goal)) : position
        let step = 1 / outputRate
        let w = Self.follow
        let fade = Float(1 - exp(-step / 0.008))
        let wanted = max(0, min(1, gain.isFinite ? gain : 0))
        for index in out.indices {
            velocity += (w * w * (target - position) - 2 * w * velocity) * step
            velocity = min(Self.fastest, max(-Self.fastest, velocity))
            position += velocity * step
            if position <= 0 || position >= end {
                position = min(end, max(0, position))
                velocity = 0
            }
            let place = position * sourceRate
            let base = min(count - 2, Int(place))
            let fraction = Float(place - Double(base))
            let sample = (Float(pcm[base]) * (1 - fraction) + Float(pcm[base + 1]) * fraction) / 32768
            let filtered = sample - lastIn + 0.995 * lastOut
            lastIn = sample
            lastOut = filtered
            let moving = Float(min(1, abs(velocity) / Self.fullSpeed))
            level += (wanted * moving - level) * fade
            out[index] = max(-1, min(1, filtered * level))
        }
    }
}
