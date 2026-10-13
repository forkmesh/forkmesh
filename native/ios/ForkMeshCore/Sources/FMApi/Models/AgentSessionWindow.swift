import FMBase
import Foundation

public struct AgentSessionList: Equatable, Hashable, Sendable {
    public var sessions: [AgentLaunchSession]
    public var finishedBeforeMs: Int

    public init(sessions: [AgentLaunchSession] = [], finishedBeforeMs: Int = 0) {
        self.sessions = sessions
        self.finishedBeforeMs = finishedBeforeMs
    }

    public init(json: JSONObject, orgScope: Bool = false) {
        self.init(
            sessions: json.maps("sessions").map { AgentLaunchSession(json: $0, orgScope: orgScope) },
            finishedBeforeMs: agentLaunchMs(json["finishedBefore"])
        )
    }

    public func keeping(_ previous: [AgentLaunchSession]) -> [AgentLaunchSession] {
        AgentLaunchSession.keepingListed(sessions, previous: previous, finishedBeforeMs: finishedBeforeMs)
    }
}

extension AgentLaunchSession {
    public func isBeyondListedWindow(finishedBeforeMs: Int) -> Bool {
        finishedBeforeMs > 0 && updatedAtMs > 0 && updatedAtMs <= finishedBeforeMs && !isActive
    }

    public static func keepingListed(
        _ listed: [AgentLaunchSession], previous: [AgentLaunchSession], finishedBeforeMs: Int
    ) -> [AgentLaunchSession] {
        if finishedBeforeMs <= 0 { return listed }
        let ids = Set(listed.map(\.sessionId))
        return listed + previous.filter {
            !ids.contains($0.sessionId) && $0.isBeyondListedWindow(finishedBeforeMs: finishedBeforeMs)
        }
    }
}
