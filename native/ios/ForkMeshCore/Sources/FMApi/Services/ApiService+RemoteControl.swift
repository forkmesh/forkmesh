// New for the native app (WS7, remote control): the relay calls the unified Agents list and its
// controls need for runs that live on nodes rather than on the desktop in reach.
//
//   GET  /api/account/agent-sessions                       every owner-scoped cloud run
//   GET  /api/repo/<o>/<r>/agent-sessions/<sid>             one run, with its history
//   POST /api/repo/<o>/<r>/agent-sessions/<sid> {action:stop}
//        stop a run: withdrawn when no node started it, else a "stop" job for its node
//
// Follow-ups keep using `steerAgentSession`. Everything goes through `agentJson`, so a refusal
// arrives as `AgentLaunchException` with the relay's own sentence.

import FMBase
import Foundation

/// What the relay did with a stop.
public struct AgentStopResult: Equatable, Sendable {
    /// True when the run had not started and ended right there; false when the node was asked.
    public let stopped: Bool
    public let outcome: String
    public let message: String

    public init(stopped: Bool, outcome: String, message: String) {
        self.stopped = stopped
        self.outcome = outcome
        self.message = message
    }

    public init(json: JSONObject) {
        self.init(stopped: json.bool("stopped"), outcome: json.str("outcome"), message: json.str("message"))
    }
}

public struct AccountDesktop: Equatable, Sendable {
    public let key: String
    public let node: String
    public let lastSeen: Int64

    public init(key: String, node: String, lastSeen: Int64 = 0) {
        self.key = key
        self.node = node
        self.lastSeen = lastSeen
    }
}

extension ApiService {
    public func listAccountDesktops() async throws -> (account: String, desktops: [AccountDesktop]) {
        if !hasSession { return ("", []) }
        let data = try await getJson(url("/api/accounts/desktops"))
        let body = data.objectValue ?? [:]
        let desktops = (body["desktops"]?.arrayValue ?? []).compactMap(\.objectValue).compactMap {
            item -> AccountDesktop? in
            let key = item.str("key")
            let node = item.str("node").lowercased()
            if key.isEmpty || node.isEmpty { return nil }
            let seen: Int64
            if case .int(let n)? = item["lastSeen"] { seen = n } else { seen = 0 }
            return AccountDesktop(key: key, node: node, lastSeen: seen)
        }
        return (body.str("account"), desktops)
    }

    /// The account's cloud runs across its repositories, newest turns only (the relay keeps the
    /// last few per session here; `agentLaunchSession` reads one in full). Signed out: none.
    public func accountAgentSessions() async throws -> AgentSessionList {
        if !hasSession { return AgentSessionList() }
        let json = try await agentJson("GET", url("/api/account/agent-sessions"))
        return await ApiParse.modelAsync(json) { AgentSessionList(json: $0) }
    }

    /// One run on the relay queue, with its full history.
    public func agentLaunchSession(
        _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool = false
    ) async throws -> AgentLaunchSession {
        let data = try await agentJson("GET", agentSessionsURL(owner, repo, sessionId: sessionId, orgScope: orgScope))
        return AgentLaunchSession(json: data.obj("session"), orgScope: orgScope)
    }

    /// Stop a run on whichever node holds it.
    public func stopAgentSession(
        _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool = false
    ) async throws -> AgentStopResult {
        let data = try await agentJson(
            "POST", agentSessionsURL(owner, repo, sessionId: sessionId, orgScope: orgScope), ["action": "stop"])
        return AgentStopResult(json: data)
    }
}
