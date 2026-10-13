import FMApi
import FMBase
import FMChat
import Foundation

public struct RunnerDesktopMove: Equatable, Sendable {
    public let agentId: Int
    public let desktopName: String
    public let branch: String
    public let fresh: Bool
    public let already: Bool
    public let runnerKept: Bool

    public init(
        agentId: Int, desktopName: String, branch: String, fresh: Bool = false, already: Bool = false,
        runnerKept: Bool = false
    ) {
        self.agentId = agentId
        self.desktopName = desktopName
        self.branch = branch
        self.fresh = fresh
        self.already = already
        self.runnerKept = runnerKept
    }

    public var message: String {
        runnerKept ? moved + " Its run on the runner could not be deleted; delete it from Agents." : moved
    }

    private var moved: String {
        if already { return "Already continuing on \(desktopName) as agent #\(agentId)." }
        if fresh {
            return "Continuing on \(desktopName) as agent #\(agentId), from the base branch: the runner's work was not on ForkMesh."
        }
        return branch.isEmpty
            ? "Continuing on \(desktopName) as agent #\(agentId)."
            : "Continuing on \(desktopName) as agent #\(agentId), on \(branch)."
    }
}

public struct RunnerDesktopMoveError: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String

    public init(_ message: String) { self.message = message }

    public var description: String { message }
}

public enum RunnerDesktopHandoff {
    public static let capability = "agents.takeover"
    public static let maxTaskUnits = 7600
    static let maxPromptUnits = 3000
    static let maxTurnUnits = 1200
    static let maxReasonUnits = 600

    public static func canMove(_ row: UnifiedAgentRow, desktopPaired: Bool) -> Bool {
        desktopPaired && !row.isDesktop && !row.outside && !row.cloudSessionId.isEmpty && !row.repoName.isEmpty
            && !row.merged
    }

    public static func runnerName(_ session: AgentLaunchSession) -> String {
        let runner = session.runner.dartTrimmed
        if !runner.isEmpty { return runner }
        let node = session.targetNode.dartTrimmed
        return node.isEmpty ? "the runner" : node
    }

    public static func params(_ session: AgentLaunchSession, row: UnifiedAgentRow, check: Bool) -> JSONObject {
        let branch = session.branchName.dartTrimmed
        let base = session.baseBranch.dartTrimmed
        var params: JSONObject = [
            "cloudSession": .string(row.cloudSessionId),
            "owner": .string(session.owner.isEmpty ? row.repoOwner : session.owner),
            "name": .string(session.repo.isEmpty ? row.repoName : session.repo),
            "branch": .string(branch.isEmpty ? row.branchName : branch),
            "base": .string(base.isEmpty ? row.baseBranch : base),
            "runner": .string(runnerName(session)),
            "task": .string(task(session)),
            "title": .string(title(session)),
        ]
        let provider = session.provider.isEmpty ? row.provider : session.provider
        if !provider.isEmpty { params["provider"] = .string(provider) }
        let model = session.requestedModel.isEmpty ? row.model : session.requestedModel
        if !model.isEmpty { params["model"] = .string(model) }
        if check { params["check"] = .bool(true) }
        return params
    }

    public static func title(_ session: AgentLaunchSession) -> String {
        let named = session.title.dartTrimmed
        let line = named.isEmpty
            ? (session.retryPrompt.components(separatedBy: "\n").first ?? "").dartTrimmed : named
        return line.isEmpty ? "Continued runner run" : clip("Continue: " + line, 120)
    }

    public static func task(_ session: AgentLaunchSession) -> String {
        let branch = session.branchName.dartTrimmed
        let base = session.baseBranch.dartTrimmed
        var head = "Continue a task that the ForkMesh runner \(runnerName(session)) started but did not finish"
        head += " (session \(session.sessionId), it ended as \"\(session.status.dartTrimmed)\")."
        let reason = session.diagnostic.dartTrimmed.isEmpty ? session.lastError.dartTrimmed : session.diagnostic.dartTrimmed
        if !reason.isEmpty { head += " It stopped with: " + clip(flat(reason), maxReasonUnits) }
        if branch.isEmpty {
            head += " It left no branch, so start from the base branch."
        } else {
            head += " Its work so far was pushed to branch \(branch)"
            head += base.isEmpty ? "." : " (started from \(base))."
            head += " If you are on that branch, check what is already done before changing anything."
        }
        let prompt = session.retryPrompt
        head += " Then finish the task.\n\nTask:\n" + (prompt.isEmpty ? "(not recorded)" : clip(prompt, maxPromptUnits))
        let lines = conversation(session)
        if lines.isEmpty { return head }
        let intro = "\n\nWhat happened on the runner:\n"
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
        return head + intro + (kept.count < lines.count ? omitted : "") + kept.reversed().joined(separator: "\n")
    }

    static func conversation(_ session: AgentLaunchSession) -> [String] {
        var lines: [String] = []
        var skippedPrompt = false
        for turn in session.history {
            let said = turn.text.dartTrimmed
            if said.isEmpty { continue }
            if turn.isUser && !skippedPrompt {
                skippedPrompt = true
                continue
            }
            let who = turn.isUser ? "Me" : (turn.role.lowercased() == "security" ? "Note" : "Agent")
            lines.append(who + ": " + clip(flat(said), maxTurnUnits))
        }
        return lines
    }

    public static func failureText(_ error: Error, repo: String, stoppedOn: String = "") -> String {
        let after = stoppedOn.isEmpty ? "" : " The run on \(stoppedOn) is stopped; Continue restarts it there."
        if let control = error as? LocalControlException {
            switch control.code {
            case "unsupported", "unknown_method":
                return "Update ForkMesh on the desktop to move runner agents to it." + after
            case "no_checkout":
                return "The desktop has no working copy of \(repo). Clone it there first, then move the agent." + after
            case "bad_branch", "bad_task", "bad_session":
                return "The desktop refused the move (\(control.code))." + after
            case "not_started":
                return (control.message.isEmpty
                    ? "The desktop could not start the agent. It says why in its Agents list."
                    : ControlErrorText.withReason("The desktop could not start the agent.", control)) + after
            default:
                return ControlErrorText.describe(error) + after
            }
        }
        if let move = error as? RunnerDesktopMoveError { return move.message }
        return String(describing: error) + after
    }

    static func flat(_ text: String) -> String {
        text.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\n", with: " ")
    }

    static func clip(_ text: String, _ limit: Int) -> String {
        if text.utf16.count <= limit { return text }
        var out = ""
        var used = 0
        for character in text {
            let units = String(character).utf16.count
            if used + units > limit - 1 { break }
            out.append(character)
            used += units
        }
        return out + "…"
    }
}
