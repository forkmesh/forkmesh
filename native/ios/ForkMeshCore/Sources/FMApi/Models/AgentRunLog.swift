import Foundation
import FMBase

public enum AgentLogTone: String, Sendable, CaseIterable {
    case command, muted, prompt, step, active, output, tool, success, warning, failure
}

public struct AgentLogLine: Equatable, Hashable, Sendable {
    public let atMs: Int
    public let tone: AgentLogTone
    public let text: String

    public init(atMs: Int = 0, tone: AgentLogTone, text: String) {
        self.atMs = atMs
        self.tone = tone
        self.text = text
    }
}

public struct AgentRunLog: Equatable, Sendable {
    public static let maxOutputLines = 400
    public static let maxPromptLines = 6

    public let lines: [AgentLogLine]
    public let isLive: Bool
    public let isStreaming: Bool
    public let host: String
    public let status: String
    public let startedAtMs: Int
    public let endedAtMs: Int

    public init(_ chain: AgentTaskChain, repo: String) {
        let s = chain.session
        let stages = chain.stages
        let running = s.normalizedStatus == "running"
        var lines: [AgentLogLine] = []

        func step(_ key: String, atMs: Int = 0) {
            guard let index = stages.firstIndex(where: { $0.key == key }) else { return }
            let stage = stages[index]
            let tone: AgentLogTone
            switch stage.state {
            case .pending: return
            case .done: tone = .step
            case .active: tone = .active
            case .failed: tone = .failure
            }
            lines.append(AgentLogLine(
                atMs: atMs, tone: tone, text: "[\(index + 1)/\(stages.count)] \(stage.label) · \(stage.detail)"))
        }

        var command = "$ forkmesh agent run --repo \(repo)"
        if !s.provider.isEmpty { command += " --provider \(s.provider)" }
        if !s.requestedModel.isEmpty { command += " --model \(s.requestedModel)" }
        if !s.targetNode.isEmpty { command += " --on \(s.targetNode)" }
        if s.createPr { command += " --pr" }
        lines.append(AgentLogLine(atMs: s.createdAtMs, tone: .command, text: command))

        step("prompt", atMs: s.createdAtMs)
        let firstUser = s.history.firstIndex { $0.isUser }
        lines += AgentRunLog.promptLines(firstUser.map { s.history[$0].text } ?? s.title, author: "", atMs: 0)
        step("mirror")
        if !s.diagnosticNote.isEmpty {
            lines.append(AgentLogLine(tone: .muted, text: "# \(s.diagnosticNote)"))
        }
        step("branch")

        var output: [AgentLogLine] = []
        let turns = firstUser.map { Array(s.history[($0 + 1)...]) } ?? s.history
        for turn in turns {
            if turn.isUser {
                output += AgentRunLog.promptLines(turn.text, author: turn.author, atMs: turn.atMs)
            } else {
                output += AgentRunLog.outputLines(turn.text, atMs: turn.atMs)
            }
        }
        let streaming = running && !s.liveText.dartTrimmed.isEmpty
        if streaming {
            output += AgentRunLog.outputLines(s.liveText, atMs: 0)
        }
        if output.count > AgentRunLog.maxOutputLines {
            let dropped = output.count - AgentRunLog.maxOutputLines
            output = [AgentLogLine(tone: .muted, text: "… \(dropped) earlier lines")] + output.suffix(AgentRunLog.maxOutputLines)
        }
        lines += output
        if running && !streaming {
            lines.append(AgentLogLine(tone: .muted, text: "# waiting for the agent's first output…"))
        }

        let ended = s.completedAtMs > 0 ? s.completedAtMs : (s.isActive ? 0 : s.updatedAtMs)
        if s.isComplete {
            let took = AgentRunLog.duration(from: s.createdAtMs, to: ended)
            lines.append(AgentLogLine(atMs: ended, tone: .success, text: took.isEmpty ? "✓ agent finished" : "✓ agent finished in \(took)"))
        } else if s.isStopped {
            lines.append(AgentLogLine(atMs: ended, tone: .warning, text: "■ run stopped"))
        } else if s.isFailed {
            let reason = s.lastError.dartTrimmed
            lines.append(AgentLogLine(atMs: ended, tone: .failure, text: reason.isEmpty ? "✗ run failed" : "✗ \(reason)"))
        }
        step("pull")
        step("merge", atMs: chain.isMerged ? s.mergedAtMs : 0)

        self.lines = lines
        self.isLive = s.isActive
        self.isStreaming = streaming
        self.host = s.targetNode.isEmpty ? "agent" : "\(s.provider.isEmpty ? "agent" : s.provider)@\(s.targetNode)"
        if chain.isMerged {
            self.status = "merged"
        } else if s.isActive {
            self.status = "live"
        } else if s.isComplete {
            self.status = "exit 0"
        } else if s.isStopped {
            self.status = "stopped"
        } else {
            self.status = "exit 1"
        }
        self.startedAtMs = s.createdAtMs
        self.endedAtMs = ended
    }

    public static func promptLines(_ text: String, author: String, atMs: Int) -> [AgentLogLine] {
        let rows = text.dartTrimmed.split(separator: "\n", omittingEmptySubsequences: true).map(String.init)
        guard !rows.isEmpty else { return [] }
        let who = author.isEmpty ? "" : "@\(author) "
        var result: [AgentLogLine] = rows.prefix(maxPromptLines).enumerated().map { offset, row in
            AgentLogLine(atMs: offset == 0 ? atMs : 0, tone: .prompt, text: offset == 0 ? "› \(who)\(row)" : "  \(row)")
        }
        if rows.count > maxPromptLines {
            result.append(AgentLogLine(tone: .muted, text: "  … \(rows.count - maxPromptLines) more lines"))
        }
        return result
    }

    public static func outputLines(_ text: String, atMs: Int) -> [AgentLogLine] {
        var result: [AgentLogLine] = []
        for raw in text.split(separator: "\n", omittingEmptySubsequences: true) {
            let row = String(raw).replacingOccurrences(of: "\t", with: "    ")
            if row.dartTrimmed.isEmpty { continue }
            let tone: AgentLogTone = row.hasPrefix("▸") ? .tool : (row == "…" ? .muted : .output)
            result.append(AgentLogLine(atMs: result.isEmpty ? atMs : 0, tone: tone, text: row))
        }
        return result
    }

    public func elapsed(nowMs: Int) -> String {
        AgentRunLog.duration(from: startedAtMs, to: endedAtMs > 0 ? endedAtMs : nowMs)
    }

    public static func duration(from start: Int, to end: Int) -> String {
        guard start > 0, end >= start else { return "" }
        let seconds = (end - start) / 1_000
        if seconds < 60 { return "\(seconds)s" }
        if seconds < 3_600 { return "\(seconds / 60)m \(String(format: "%02d", seconds % 60))s" }
        return "\(seconds / 3_600)h \(String(format: "%02d", (seconds % 3_600) / 60))m"
    }
}
