import Foundation

public enum DirectCallRoute: String, Equatable, Sendable {
    case direct
    case relay

    public static let hint = "Calls go directly between your devices when possible."

    public var note: String {
        switch self {
        case .direct: return "Direct · peer-to-peer, no server in between"
        case .relay: return "Relayed · through a TURN relay server (third party)"
        }
    }
}

public struct DirectCallToneBurst: Equatable, Sendable {
    public let start: Double
    public let length: Double

    public init(start: Double, length: Double) {
        self.start = start
        self.length = length
    }
}

public enum DirectCallTones {
    public static let rate = 22050
    public static let ringbackCycle: Double = 6
    public static let ringCycle: Double = 3
    public static let ringbackHz: [Double] = [440, 480]
    public static let ringHz: [Double] = [400, 450]
    public static let ringbackBursts = [DirectCallToneBurst(start: 0, length: 2)]
    public static let ringBursts = [
        DirectCallToneBurst(start: 0, length: 0.4), DirectCallToneBurst(start: 0.6, length: 0.4),
    ]

    public static func ringback(rate: Int = DirectCallTones.rate) -> [Int16] {
        render(frequencies: ringbackHz, bursts: ringbackBursts, cycle: ringbackCycle, rate: rate)
    }

    public static func ring(rate: Int = DirectCallTones.rate) -> [Int16] {
        render(frequencies: ringHz, bursts: ringBursts, cycle: ringCycle, rate: rate)
    }

    public static func render(frequencies: [Double], bursts: [DirectCallToneBurst], cycle: Double, rate: Int) -> [Int16] {
        let perSecond = Double(rate)
        let total = max(0, Int(cycle * perSecond))
        var pcm = [Int16](repeating: 0, count: total)
        guard !frequencies.isEmpty, total > 0 else { return pcm }
        let fade = max(1, perSecond * 0.01)
        let level = 0.35 * 32767 / Double(frequencies.count)
        for burst in bursts {
            let first = max(0, Int(burst.start * perSecond))
            let count = min(Int(burst.length * perSecond), total - first)
            guard count > 0 else { continue }
            for index in 0..<count {
                let time = Double(first + index) / perSecond
                let edge = min(1, Double(index) / fade, Double(count - 1 - index) / fade)
                var sample = 0.0
                for frequency in frequencies { sample += sin(2 * Double.pi * frequency * time) }
                pcm[first + index] = Int16(max(-32767, min(32767, (sample * level * edge).rounded())))
            }
        }
        return pcm
    }
}
