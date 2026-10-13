import FMApi
import FMBase
import FMChat
import FMCode
import FMControl
import Foundation

@MainActor
public protocol AgentHandoffDesktop: AnyObject {
    var desktopName: String { get }
    func handoff(owner: String, name: String, branch: String, base: String, task: String, title: String) async throws -> Int
}

@MainActor
public final class HubAgentHandoffDesktop: AgentHandoffDesktop {
    private let hub: @MainActor () -> DesktopControlHub?

    public init(hub: @escaping @MainActor () -> DesktopControlHub?) { self.hub = hub }

    public var desktopName: String {
        guard let desktop = hub()?.desktop, !desktop.name.isEmpty else { return "your desktop" }
        return desktop.name
    }

    public func handoff(owner: String, name: String, branch: String, base: String, task: String, title: String) async throws -> Int {
        guard let hub = hub() else { throw LocalControlException("not_connected") }
        let params: JSONObject = [
            "owner": .string(owner), "name": .string(name), "branch": .string(branch),
            "base": .string(base), "task": .string(task), "title": .string(title),
        ]
        return try await hub.request("agents.handoff", params, timeout: 60).int("id")
    }
}

public struct AgentDesktopMove: Equatable, Sendable {
    public let agentId: Int
    public let desktopName: String
    public let branch: String

    public var message: String { "Continuing on \(desktopName) as agent #\(agentId), on \(branch)." }
}

public enum AgentDesktopHandoff {
    public static let maxTaskUnits = 7600
    static let maxPromptUnits = 3000
    static let maxLineUnits = 600

    public static func publishedRepo(_ record: LocalRepoRecord?) -> (owner: String, name: String)? {
        let published = (record?.publishedAs ?? "").dartTrimmed
        let parts = published.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        guard parts.count == 2, !parts[0].isEmpty, !parts[1].isEmpty else { return nil }
        return (parts[0], parts[1])
    }

    public static func title(prompt: String) -> String {
        let first = (prompt.components(separatedBy: "\n").first ?? "").dartTrimmed
        let line = first.isEmpty ? "Agent moved from the phone" : first
        return line.utf16.count > 120 ? AgentText.prefixUTF16(line, 119) + "…" : line
    }

    public static func task(prompt: String, events: [AgentEvent], branch: String, base: String) -> String {
        let trimmedPrompt = prompt.dartTrimmed
        let shownPrompt = trimmedPrompt.utf16.count > maxPromptUnits
            ? AgentText.prefixUTF16(trimmedPrompt, maxPromptUnits) + "…"
            : trimmedPrompt
        let head = """
            Continue a task that a coding agent started on my phone. Its work so far is committed on \
            branch \(branch) (started from \(base)), and you are working on that branch now. Check what \
            is already done before changing anything, then finish the task.

            Task:
            \(shownPrompt)
            """
        let lines = conversation(events, prompt: trimmedPrompt)
        if lines.isEmpty { return head }
        let intro = "\n\nWhat happened on the phone:\n"
        let omitted = "… earlier steps omitted\n"
        let budget = maxTaskUnits - head.utf16.count - intro.utf16.count - omitted.utf16.count
        var kept: [String] = []
        var used = 0
        for line in lines.reversed() {
            let cost = line.utf16.count + 1
            if used + cost > budget { break }
            kept.append(line)
            used += cost
        }
        if kept.isEmpty { return head }
        let body = kept.reversed().joined(separator: "\n")
        return head + intro + (kept.count < lines.count ? omitted : "") + body
    }

    static func conversation(_ events: [AgentEvent], prompt: String) -> [String] {
        var lines: [String] = []
        var skippedPrompt = false
        for event in events {
            switch event {
            case .userMessage(let text):
                let said = text.dartTrimmed
                if !skippedPrompt && said == prompt {
                    skippedPrompt = true
                    continue
                }
                skippedPrompt = true
                if !said.isEmpty { lines.append("Me: " + clip(said)) }
            case .assistantMessage(let text):
                let said = text.dartTrimmed
                if !said.isEmpty { lines.append("Agent: " + clip(said)) }
            case .toolCall(let call):
                let target = ["path", "command", "pattern", "query", "branch", "message"]
                    .lazy.compactMap { call.arguments[$0]?.stringValue }
                    .first { !$0.dartTrimmed.isEmpty }
                lines.append("Tool: " + call.name + (target.map { " " + clip($0.dartTrimmed) } ?? ""))
            case .toolResult(let result):
                if result.isError { lines.append("Tool error: " + clip(result.output.dartTrimmed)) }
            case .notice(let text, let isError):
                if isError { lines.append("Note: " + clip(text.dartTrimmed)) }
            case .done(let done):
                lines.append("Run ended: " + stopText(done.reason))
            }
        }
        return lines
    }

    static func stopText(_ reason: AgentStopReason) -> String {
        switch reason {
        case .completed: return "the agent said it was done"
        case .cancelled: return "stopped to move it to the desktop"
        case .turnLimit: return "hit its turn limit"
        case .error: return "an error"
        case .refused: return "the model refused"
        }
    }

    static func clip(_ text: String) -> String {
        let flat = text.replacingOccurrences(of: "\n", with: " ")
        return flat.utf16.count > maxLineUnits ? AgentText.prefixUTF16(flat, maxLineUnits - 1) + "…" : flat
    }

    public static func failureText(_ error: Error, repo: String, branch: String) -> String {
        if let control = error as? LocalControlException {
            switch control.code {
            case "unsupported", "unknown_method":
                return "Update ForkMesh on the desktop to move agents to it."
            case "no_checkout":
                return "The desktop has no working copy of \(repo). Clone it there first, then move the agent."
            case "branch_not_found":
                return "The desktop could not fetch \(branch) from ForkMesh. The branch is pushed; try again in a moment."
            case "bad_branch", "bad_task":
                return "The desktop refused the move (\(control.code))."
            case "not_started":
                let reason = control.message.trimmingCharacters(in: .whitespacesAndNewlines)
                return reason.isEmpty
                    ? "The desktop could not start the agent. It says why in its Agents list."
                    : "The desktop could not start the agent. \(reason)"
            default:
                return "Could not reach your desktop. Open ForkMesh there and try again."
            }
        }
        return dartErrorText(error)
    }
}
