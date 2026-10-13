// The runs the Watch screen can follow (a desktop session read through the relay, one read over the local
// network, the run on this phone) as FMAgentWatch targets.
//
// FMAgents does not depend on FMChat, so the local-network target takes the
// LocalControlService calls as closures; the app adapts the service.

import FMAgentWatch
import FMApi
import FMBase
import Foundation

/// Holds a non-Sendable reference for a `@Sendable` load closure. The loads
/// only call async methods that do their own isolation.
private final class Carried<Value>: @unchecked Sendable {
    let value: Value
    init(_ value: Value) { self.value = value }
}

/// Thrown when the relay holds the transcript sealed to another owner device.
public struct AgentWatchSealedError: Error, CustomStringConvertible {
    public init() {}
    public var description: String { "Sealed for another owner device." }
}

/// A desktop session read through the relay: the owner-sealed transcript the
/// desktop pushes at each step. `session`'s own status stands in when the
/// transcript does not carry one.
public func relayWatchTarget(
    _ api: AgentsApi, _ repo: Repository, _ session: AgentSession, ownerAccount: String
) -> AgentWatchTarget {
    let carried = Carried(api)
    let owner = repo.owner
    let name = repo.name
    let id = session.id
    let fallbackStatus = session.status
    let merged = session.merged
    return AgentWatchTarget(
        key: "relay:\(repo.fullName)#\(session.id)",
        id: session.id,
        title: session.displayTitle,
        subtitle: repo.fullName,
        project: repo.name,
        provider: session.provider,
        model: session.model,
        turns: session.numTurns,
        tokens: session.totalTokens,
        load: {
            let detail = try await carried.value.agentTranscript(owner, name, id, ownerAccount: ownerAccount)
            if detail.sealedForOtherDevice { throw AgentWatchSealedError() }
            let status = detail.status.isEmpty ? fallbackStatus : detail.status
            return AgentWatchSnapshot(
                status: merged ? "done" : status, beats: beatsFromTranscript(detail.transcript))
        })
}

/// A desktop session read directly over the local network.
///
/// - Parameters:
///   - transcript: `LocalControlService.transcript(sessionId)`.
///   - currentStatus: the status in the service's latest Agents list, the
///     freshest there is; nil when the list no longer has the session.
public func localNetworkWatchTarget(
    repoLabel: String,
    name: String,
    title: String,
    prompt: String,
    session: AgentSession,
    transcript: @escaping @Sendable (Int) async throws -> String,
    currentStatus: @escaping @Sendable (Int) async -> String?
) -> AgentWatchTarget {
    let id = session.id
    let fallbackStatus = session.status
    return AgentWatchTarget(
        key: "lan:\(repoLabel)#\(session.id)",
        id: session.id,
        title: title,
        subtitle: repoLabel,
        project: name,
        provider: session.provider,
        model: session.model,
        prompt: prompt,
        turns: session.numTurns,
        tokens: session.totalTokens,
        load: {
            let text = try await transcript(id)
            let status = await currentStatus(id) ?? fallbackStatus
            return AgentWatchSnapshot(status: status, beats: beatsFromTranscript(text))
        })
}

/// The run on this phone: its events are already here, nothing is fetched.
@MainActor
public func deviceWatchTarget(_ sessions: AgentSessionService, _ repoName: String) -> AgentWatchTarget {
    let prompt = sessions.lastRun?.task.prompt.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    let firstLine = (prompt.components(separatedBy: "\n").first ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
    let project: String
    if let slash = repoName.range(of: "/", options: .backwards) {
        project = String(repoName[slash.upperBound...])
    } else {
        project = repoName
    }
    let carried = Carried(sessions)
    return AgentWatchTarget(
        key: "device:\(repoName)",
        id: 0,
        title: firstLine.isEmpty ? repoName : firstLine,
        subtitle: "On this phone",
        project: project,
        prompt: prompt,
        load: {
            await MainActor.run { deviceSnapshot(carried.value) }
        })
}

@MainActor
private func deviceSnapshot(_ sessions: AgentSessionService) -> AgentWatchSnapshot {
    let run = sessions.lastRun
    let events = run?.events ?? []
    let running = run != nil && !(run?.isComplete ?? true)
    let failed = events.contains { event in
        guard case .done(let done) = event else { return false }
        return done.reason == .error || done.reason == .refused
    }
    return AgentWatchSnapshot(
        status: running ? "running" : failed ? "failed" : "done", beats: beatsFromAgentEvents(events))
}

/// One line, cut to 90 UTF-16 units with an ellipsis.
private func clipLine(_ text: String, _ limit: Int = 90) -> String {
    var line = ""
    var space = false
    for scalar in text.unicodeScalars {
        if scalar.properties.isWhitespace {
            space = !line.isEmpty
        } else {
            if space { line.append(" ") }
            space = false
            line.unicodeScalars.append(scalar)
        }
    }
    let units = Array(line.utf16)
    if units.count <= limit { return line }
    return String(decoding: units[0..<(limit - 1)], as: UTF16.self) + "\u{2026}"
}

private let refusals = ["denied", "permission", "rejected", "doesn't want", "not allowed", "blocked", "refused"]

/// What came back from a tool, as the desktop hears it: an error, a refusal,
/// or an answer.
private func resultMotion(_ isError: Bool, _ output: String) -> AgentMotion {
    if !isError { return .arrive }
    let units = Array(output.utf16)
    let head = units.count > 2000 ? String(decoding: units[0..<2000], as: UTF16.self) : output
    let lower = head.lowercased()
    return refusals.contains { lower.contains($0) } ? .denied : .fumble
}

/// The beats of a run on this phone, one per event. Append-only, like the
/// events themselves, so the prefix rule of `beatsFromTranscript` holds.
public func beatsFromAgentEvents(_ events: [AgentEvent]) -> [WatchBeat] {
    var beats: [WatchBeat] = []
    for event in events {
        switch event {
        case .userMessage(let text):
            beats.append(WatchBeat(.prompt, clipLine(text)))
        case .assistantMessage(let text):
            if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                beats.append(WatchBeat(.speak, clipLine(text), bytes: text.utf16.count, said: plainProse(text)))
            }
        case .toolCall(let call):
            let command: String? = call.arguments["command"].flatMap { $0.isNull ? nil : $0.text }
            let path: String = call.arguments["path"].flatMap { $0.isNull ? nil : $0.text } ?? ""
            beats.append(WatchBeat(
                motionForTool(call.name, command: command ?? ""), clipLine("\(call.name) \(command ?? path)")))
        case .toolResult(let result):
            beats.append(WatchBeat(
                resultMotion(result.isError, result.output), clipLine("\(result.name): \(result.output)")))
        case .notice(let text, let isError):
            beats.append(WatchBeat(isError ? .notice : .system, clipLine(text)))
        case .done(let done):
            let failed = done.reason == .error || done.reason == .refused
            beats.append(WatchBeat(
                failed ? .fail : .finish,
                "\(done.reason.rawValue) after \(done.turns) \(done.turns == 1 ? "turn" : "turns")"))
        }
    }
    return beats
}
