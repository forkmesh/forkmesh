import Foundation

public enum AgentTokenPace: Int, Comparable, CaseIterable, Sendable {
    case idle, slow, steady, fast, blazing

    public static let inputScale = 10.0

    public static func < (lhs: AgentTokenPace, rhs: AgentTokenPace) -> Bool { lhs.rawValue < rhs.rawValue }

    public static func of(perSecond: Double, input: Bool) -> AgentTokenPace {
        let rate = input ? perSecond / inputScale : perSecond
        if rate < 1 { return .idle }
        if rate < 15 { return .slow }
        if rate < 60 { return .steady }
        if rate < 200 { return .fast }
        return .blazing
    }

    public var travel: Double {
        switch self {
        case .idle: return 0
        case .slow: return 4
        case .steady: return 8
        case .fast: return 14
        case .blazing: return 22
        }
    }
}

public struct AgentTokenSampler: Equatable, Sendable {
    public static let longestGapMs = 8_000

    public private(set) var inPerSecond = 0.0
    public private(set) var outPerSecond = 0.0
    private var lastIn = -1
    private var lastOut = -1
    private var lastMs = 0

    public init() {}

    public var inPace: AgentTokenPace { .of(perSecond: inPerSecond, input: true) }
    public var outPace: AgentTokenPace { .of(perSecond: outPerSecond, input: false) }

    public mutating func sample(tokensIn: Int, tokensOut: Int, atMs: Int) {
        defer {
            lastIn = tokensIn
            lastOut = tokensOut
            lastMs = atMs
        }
        guard lastIn >= 0, atMs > lastMs else { return }
        let seconds = Double(min(atMs - lastMs, Self.longestGapMs)) / 1000
        inPerSecond = Self.blend(inPerSecond, Double(max(tokensIn - lastIn, 0)) / seconds)
        outPerSecond = Self.blend(outPerSecond, Double(max(tokensOut - lastOut, 0)) / seconds)
    }

    public mutating func settle() {
        inPerSecond = 0
        outPerSecond = 0
    }

    private static func blend(_ old: Double, _ new: Double) -> Double {
        old == 0 || new == 0 ? new : (old + new) / 2
    }
}
