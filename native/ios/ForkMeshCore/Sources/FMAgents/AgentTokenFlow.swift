import FMControl
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

    public static func travel(perSecond: Double, input: Bool) -> Double {
        let rate = input ? perSecond / inputScale : perSecond
        guard rate >= 1 else { return 0 }
        return min(maxTravel, 4 * (1 + log10(rate)))
    }

    public static let maxTravel = 24.0

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
    public static let halfLifeSeconds = 1.5
    public static let floorPerSecond = 0.5

    public private(set) var inPerSecond = 0.0
    public private(set) var outPerSecond = 0.0
    private var lastIn = -1
    private var lastOut = -1
    private var lastMs = 0

    public init() {}

    public var inPace: AgentTokenPace { .of(perSecond: inPerSecond, input: true) }
    public var outPace: AgentTokenPace { .of(perSecond: outPerSecond, input: false) }
    public var inTravel: Double { AgentTokenPace.travel(perSecond: inPerSecond, input: true) }
    public var outTravel: Double { AgentTokenPace.travel(perSecond: outPerSecond, input: false) }

    public mutating func sample(tokensIn: Int, tokensOut: Int, atMs: Int) {
        defer {
            lastIn = tokensIn
            lastOut = tokensOut
            lastMs = atMs
        }
        guard lastIn >= 0, atMs > lastMs else { return }
        let seconds = Double(min(atMs - lastMs, Self.longestGapMs)) / 1000
        inPerSecond = Self.blend(inPerSecond, grown: tokensIn - lastIn, seconds: seconds)
        outPerSecond = Self.blend(outPerSecond, grown: tokensOut - lastOut, seconds: seconds)
    }

    public mutating func hold(atMs: Int) {
        guard lastIn >= 0 else { return }
        sample(tokensIn: lastIn, tokensOut: lastOut, atMs: atMs)
    }

    public mutating func settle() {
        inPerSecond = 0
        outPerSecond = 0
    }

    private static func blend(_ old: Double, grown: Int, seconds: Double) -> Double {
        if grown < 0 { return 0 }
        if grown == 0 {
            let faded = old * pow(0.5, seconds / halfLifeSeconds)
            return faded < floorPerSecond ? 0 : faded
        }
        let new = Double(grown) / seconds
        return old == 0 ? new : (old + new) / 2
    }
}

public struct AgentTokenSeries: Equatable, Sendable {
    public static let seconds = 60

    public private(set) var input: [Double] = []
    public private(set) var output: [Double] = []
    private var last: [String: [Int]] = [:]

    public init() {}

    public mutating func tick(_ totals: [String: [Int]]) {
        var grownIn = 0
        var grownOut = 0
        for (key, now) in totals where now.count == 2 {
            guard let before = last[key], before.count == 2 else { continue }
            grownIn += max(0, now[0] - before[0])
            grownOut += max(0, now[1] - before[1])
        }
        last = totals
        input = Self.pushed(input, Double(grownIn))
        output = Self.pushed(output, Double(grownOut))
    }

    public static func totals(desktop: [Int], rows: [UnifiedAgentRow]) -> [String: [Int]] {
        let fed = desktop.count == 2
        var totals: [String: [Int]] = fed ? ["desktop": desktop] : [:]
        for row in rows where !(fed && row.isDesktop) {
            totals["row:\(row.id)"] = [row.tokensIn, row.tokensOut]
        }
        return totals
    }

    private static func pushed(_ values: [Double], _ value: Double) -> [Double] {
        Array((values + [value]).suffix(seconds))
    }
}
