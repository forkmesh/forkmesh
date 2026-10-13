import FMApi
import FMBase
import FMControl
import FMLocalAI
import Foundation

extension ForkMeshToolbox {
    public nonisolated static let transcriptTailChars = 4000

    func agentRows() async -> [UnifiedAgentRow]? {
        guard let control = sources.agentControl() else { return nil }
        await control.refresh()
        return control.rows
    }

    func findAgent(_ args: JSONObject) async throws -> UnifiedAgentRow {
        guard let wanted = AssistantText.argument(args, "id") else { throw AssistantError("give the agent's id from list_agents") }
        guard let rows = await agentRows() else { throw AssistantError("agent control is not available in this build") }
        let lower = wanted.lowercased()
        let hit = rows.first { $0.id.lowercased() == lower }
            ?? rows.first { $0.cloudSessionId.lowercased() == lower }
            ?? rows.first { $0.localId > 0 && "\($0.localId)" == wanted }
            ?? rows.first { $0.title.lowercased().contains(lower) }
        guard let hit else { throw AssistantError("no agent matches \"\(wanted)\"; call list_agents for the ids") }
        return hit
    }

    func agentName(_ args: JSONObject) async -> String {
        guard let row = try? await findAgent(args) else { return AssistantText.argument(args, "id") ?? "(unknown)" }
        return "\"\(row.title)\"\(row.repoLabelText.isEmpty ? "" : " in \(row.repoLabelText)")"
    }

    func listAgents(_ args: JSONObject) async throws -> String {
        if AssistantText.argument(args, "id") != nil {
            return try await readAgent(args)
        }
        let query = AssistantText.argument(args, "query")
        let runningOnly = args["running_only"]?.isTrue == true
        if var rows = await agentRows() {
            rows.sort { $0.updatedAtMs > $1.updatedAtMs }
            if runningOnly { rows = rows.filter(\.isActive) }
            if let query { rows = rows.filter { AssistantText.matches(query, [$0.title, $0.repoLabelText, $0.node, $0.branchName, $0.provider]) } }
            return AssistantText.list("Agents on your desktop and in the cloud: \(rows.count)", rows.map { AssistantText.agentRowLine($0, timeZone) })
        }
        var agents = try await sources.agents().sorted { $0.updatedAtMs > $1.updatedAtMs }
        if runningOnly { agents = agents.filter(AssistantText.isRunning) }
        if let query { agents = agents.filter { AssistantText.matches(query, [$0.title, $0.repo, $0.owner, $0.branchName, $0.provider]) } }
        return AssistantText.list("Agent runs: \(agents.count)", agents.map { AssistantText.agentLine($0, timeZone) })
    }

    func readAgent(_ args: JSONObject) async throws -> String {
        let row = try await findAgent(args)
        guard let control = sources.agentControl() else { throw AssistantError("agent control is not available in this build") }
        let delta = try await control.transcript(row, since: 0)
        let text = delta.text.trimmingCharacters(in: .whitespacesAndNewlines)
        let tail = text.count > ForkMeshToolbox.transcriptTailChars
            ? "… " + String(text.suffix(ForkMeshToolbox.transcriptTailChars))
            : text
        return "Agent " + AssistantText.agentRowLine(row, timeZone) + "\nStatus: \(delta.status.isEmpty ? row.statusLabel : delta.status)\(delta.active ? " (working)" : "")\n\nTranscript, latest part:\n" + (tail.isEmpty ? "(empty)" : tail)
    }

    func messageAgent(_ args: JSONObject) async throws -> String {
        guard let text = AssistantText.argument(args, "text") else { throw AssistantError("message_agent needs text") }
        let row = try await findAgent(args)
        guard let control = sources.agentControl() else { throw AssistantError("agent control is not available in this build") }
        try await control.prompt(row, text, images: [])
        let notice = control.promptNotice
        return "Sent to \"\(row.title)\"." + (notice.isEmpty ? "" : " \(notice)")
    }

    func stopAgent(_ args: JSONObject) async throws -> String {
        let row = try await findAgent(args)
        guard let control = sources.agentControl() else { throw AssistantError("agent control is not available in this build") }
        try await control.stop(row)
        return "Stopped \"\(row.title)\"."
    }
}

extension UnifiedAgentRow {
    var repoLabelText: String {
        repoOwner.isEmpty || repoName.isEmpty ? repoName : "\(repoOwner)/\(repoName)"
    }
}

struct AssistantError: Error, CustomStringConvertible {
    let description: String

    init(_ description: String) { self.description = description }
}
