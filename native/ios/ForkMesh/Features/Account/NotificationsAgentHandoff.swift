// The one place the notifications screen reaches into the Code and Agents
// areas ("Send to agent"): the phone's repositories (LocalRepoStore), the
// repository whose agent session is running (AgentSessionService.runningRepo)
// and the screen a ping opens the agent on (seams:
// `LocalRepoScreen(record:agentPrompt:)`).
//
// The store and session types come from FMCode / FMAgents
// (`LocalRepoStore.ensureLoaded`, `.records`, `LocalRepoRecord.name`,
// `.publishedAs`, `AgentSessionService.runningRepo`). If they are renamed,
// this file is the only one to adjust.

import FMAgents
import FMApi
import FMBase
import FMCode
import Foundation
import SwiftUI
import UIKit

/// One on-device repository a ping can go to.
struct PingAgentCandidate: Identifiable {
    let record: LocalRepoRecord
    let name: String
    let publishedAs: String?

    var id: String {
        return name
    }

    var summary: PingRepoCandidate {
        return PingRepoCandidate(name: name, publishedAs: publishedAs)
    }
}

/// Where "Send to agent" leads: the repository and the ping waiting in the
/// agent's composer.
struct PingAgentTarget: Identifiable, Hashable {
    let id: UUID = UUID()
    let candidate: PingAgentCandidate
    let prompt: String

    static func == (lhs: PingAgentTarget, rhs: PingAgentTarget) -> Bool {
        return lhs.id == rhs.id
    }

    func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }
}

@MainActor
enum PingAgentHandoff {
    /// The repositories on this phone; empty without a store or a session
    /// service.
    static func candidates() async -> [PingAgentCandidate] {
        guard let store = Services.get(LocalRepoStore.self), Services.get(AgentSessionService.self) != nil else {
            return []
        }
        await store.ensureLoaded()
        return store.records.map { (record: LocalRepoRecord) -> PingAgentCandidate in
            PingAgentCandidate(record: record, name: record.name, publishedAs: record.publishedAs)
        }
    }

    /// The repository whose agent session is running, if any.
    static var runningRepo: String? {
        return Services.get(AgentSessionService.self)?.runningRepo
    }

    /// The agent screen on `target`'s repository, with the ping in its composer.
    static func destination(_ target: PingAgentTarget) -> some View {
        return LocalRepoScreen(record: target.candidate.record, agentPrompt: target.prompt)
    }
}

enum PingAgentRoute {
    case copied
    case open(PingAgentTarget)
    case pick(PingRepoPick)
}

extension PingAgentHandoff {
    static func route(_ notification: ForkNotification) async -> PingAgentRoute {
        let prompt: String = pingAgentPrompt(notification)
        let found: [PingAgentCandidate] = await candidates()
        let choice: PingRepoChoice = pingRepoChoice(
            notification.repo, candidates: found.map { $0.summary }, runningRepo: runningRepo)
        switch choice {
        case .copy:
            UIPasteboard.general.string = prompt
            return .copied
        case .open(let index):
            return .open(PingAgentTarget(candidate: found[index], prompt: prompt))
        case .pick(let order):
            return .pick(PingRepoPick(candidates: order.map { found[$0] }, prompt: prompt))
        }
    }
}
