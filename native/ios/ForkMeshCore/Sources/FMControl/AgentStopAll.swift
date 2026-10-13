import Foundation

public struct AgentStopAllOutcome: Equatable, Sendable {
    public let stopped: Int
    public let failed: Int
    public let error: String

    public init(stopped: Int, failed: Int, error: String = "") {
        self.stopped = stopped
        self.failed = failed
        self.error = error
    }

    public var message: String {
        if stopped == 0 && failed == 0 { return "No agents are running." }
        if failed == 0 { return "Asked \(AgentStopAll.count(stopped)) to stop." }
        let reason = error.isEmpty ? "" : " " + error
        if stopped == 0 { return "Could not stop \(AgentStopAll.count(failed)).\(reason)" }
        return "Asked \(AgentStopAll.count(stopped)) to stop; \(failed) did not.\(reason)"
    }
}

public enum AgentStopAll {
    public static func targets(_ rows: [UnifiedAgentRow]) -> [UnifiedAgentRow] {
        rows.filter { $0.isActive && $0.canStop && !$0.outside }
    }

    public static func count(_ n: Int) -> String {
        n == 1 ? "1 agent" : "\(n) agents"
    }

    public static func question(_ n: Int) -> String {
        n == 1 ? "Stop the running agent?" : "Stop all \(n) running agents?"
    }
}

extension AgentControlling {
    public func stopAll() async -> AgentStopAllOutcome {
        let targets: [UnifiedAgentRow] = AgentStopAll.targets(rows)
        var stopped = 0
        var error = ""
        for row in targets {
            do {
                try await stop(row)
                stopped += 1
            } catch let failure {
                if error.isEmpty { error = ControlErrorText.describe(failure) }
            }
        }
        return AgentStopAllOutcome(stopped: stopped, failed: targets.count - stopped, error: error)
    }
}
