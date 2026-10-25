import FMBase
import FMControl
import Foundation
import Observation

public enum DesktopVital: String, CaseIterable, Sendable {
    case memory, cpu, disk, swap

    public var title: String {
        switch self {
        case .memory: return "Desktop memory"
        case .cpu: return "Desktop CPU"
        case .disk: return "Desktop disk"
        case .swap: return "Desktop swap"
        }
    }

    public var limits: (warning: Double, bad: Double) { (75, 90) }

    public func tone(_ values: [Double]) -> DesktopVitalTone? {
        guard let worst = values.filter({ !$0.isNaN }).max() else { return nil }
        let (warning, bad) = limits
        return worst >= bad ? .bad : worst >= warning ? .warning : .good
    }

    public func format(_ value: Double?) -> String {
        guard let value, !value.isNaN else { return "unknown" }
        return "\(Int(value.rounded()))%"
    }
}

public enum DesktopVitalTone: Sendable, Equatable {
    case good, warning, bad
}

@MainActor
@Observable
public final class DesktopAgentTokens {
    public internal(set) var totals: [Int] = []
}

@MainActor
@Observable
public final class DesktopVitals {
    public static let seconds = 30

    public private(set) var series: [DesktopVital: [Double]] = [:]
    public private(set) var watching = false
    public private(set) var tokensIn: [Double] = []
    public private(set) var tokensOut: [Double] = []
    public private(set) var tokensKnown = false
    @ObservationIgnored private let family: DesktopControlFamily
    @ObservationIgnored private var tapId: UUID?
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var lastTokens: [Double]?
    @ObservationIgnored private var agents: [Int: DesktopAgentTokens] = [:]

    public init(channel: DesktopControlCalling) {
        family = DesktopControlFamily(.diagnostics, channel: channel)
        tapId = family.addEventTap { [weak self] suffix, data in
            if suffix == "vitals" { self?.append(data) }
        }
    }

    public func values(_ vital: DesktopVital) -> [Double] { series[vital] ?? [] }

    public func latest(_ vital: DesktopVital) -> Double? { values(vital).last { !$0.isNaN } }

    public func agentTokens(_ session: Int) -> DesktopAgentTokens {
        if let known = agents[session] { return known }
        let made = DesktopAgentTokens()
        agents[session] = made
        return made
    }

    public nonisolated static func step(from last: [Double]?, to now: [Double]) -> [Double] {
        guard let last, last.count == now.count else { return now.map { _ in .nan } }
        return zip(last, now).map { (before: Double, after: Double) -> Double in
            before < 0 || after < 0 ? .nan : max(0, after - before)
        }
    }

    public func watch() async {
        generation &+= 1
        let mine = generation
        do {
            let history = try await family.call("watch", [:], timeout: 15)
            guard mine == generation else { return }
            var next: [DesktopVital: [Double]] = [:]
            for vital in DesktopVital.allCases {
                next[vital] = Array(history.list(vital.rawValue).map { DesktopVitals.reading($0) }.suffix(DesktopVitals.seconds))
            }
            series = next
            forgetTokens()
            watching = true
        } catch {
            guard mine == generation else { return }
            series = [:]
            watching = false
        }
    }

    public func unwatch() async {
        generation &+= 1
        let was = watching
        watching = false
        series = [:]
        forgetTokens()
        if was { _ = try? await family.call("unwatch", [:], timeout: 10) }
    }

    public func clear() {
        generation &+= 1
        watching = false
        series = [:]
        forgetTokens()
    }

    private func forgetTokens() {
        lastTokens = nil
        tokensIn = []
        tokensOut = []
        tokensKnown = false
    }

    private func append(_ data: JSONObject) {
        guard watching else { return }
        for vital in DesktopVital.allCases {
            var values = series[vital] ?? []
            values.append(DesktopVitals.reading(data[vital.rawValue]))
            if values.count > DesktopVitals.seconds { values.removeFirst(values.count - DesktopVitals.seconds) }
            series[vital] = values
        }
        appendTokens(data)
    }

    private func appendTokens(_ data: JSONObject) {
        guard let into = data["tokensIn"]?.doubleValue, let out = data["tokensOut"]?.doubleValue else { return }
        let now = [into, out]
        let step = DesktopVitals.step(from: lastTokens, to: now)
        lastTokens = now
        tokensIn = DesktopVitals.pushed(tokensIn, step[0])
        tokensOut = DesktopVitals.pushed(tokensOut, step[1])
        if !tokensKnown { tokensKnown = true }
        for (key, value) in data["agentTokens"]?.objectValue ?? [:] {
            let pair = value.arrayValue?.compactMap(\.intValue) ?? []
            guard let session = Int(key), pair.count == 2 else { continue }
            let box = agentTokens(session)
            if box.totals != pair { box.totals = pair }
        }
    }

    private static func pushed(_ values: [Double], _ value: Double) -> [Double] {
        Array((values + [value]).suffix(seconds))
    }

    private static func reading(_ value: JSONValue?) -> Double {
        guard let number = value?.doubleValue, number >= 0 else { return .nan }
        return min(number, 100)
    }
}
