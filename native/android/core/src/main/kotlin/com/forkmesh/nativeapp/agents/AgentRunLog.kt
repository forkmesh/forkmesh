package com.forkmesh.nativeapp.agents

enum class AgentLogTone { Command, Muted, Prompt, Step, Active, Output, Tool, Success, Warning, Failure }

data class AgentLogLine(val atMs: Long = 0, val tone: AgentLogTone, val text: String)

class AgentRunLog(chain: AgentTaskChain, repo: String) {
    val lines: List<AgentLogLine>
    val isLive: Boolean
    val isStreaming: Boolean
    val host: String
    val status: String
    val startedAtMs: Long
    val endedAtMs: Long

    init {
        val s = chain.session
        val stages = chain.stages
        val running = s.normalizedStatus == "running"
        val out = ArrayList<AgentLogLine>()

        fun step(key: String, atMs: Long = 0) {
            val index = stages.indexOfFirst { it.key == key }
            if (index < 0) return
            val stage = stages[index]
            val tone = when (stage.state) {
                AgentStageState.Pending -> return
                AgentStageState.Done -> AgentLogTone.Step
                AgentStageState.Active -> AgentLogTone.Active
                AgentStageState.Failed -> AgentLogTone.Failure
            }
            out += AgentLogLine(atMs, tone, "[${index + 1}/${stages.size}] ${stage.label} · ${stage.detail}")
        }

        var command = "$ forkmesh agent run --repo $repo"
        if (s.provider.isNotEmpty()) command += " --provider ${s.provider}"
        if (s.requestedModel.isNotEmpty()) command += " --model ${s.requestedModel}"
        if (s.targetNode.isNotEmpty()) command += " --on ${s.targetNode}"
        if (s.createPr) command += " --pr"
        out += AgentLogLine(s.createdAtMs, AgentLogTone.Command, command)

        step("prompt", s.createdAtMs)
        val firstUser = s.history.indexOfFirst { it.isUser }
        out += promptLines(if (firstUser >= 0) s.history[firstUser].text else s.title, "", 0)
        step("mirror")
        if (s.diagnosticNote.isNotEmpty()) out += AgentLogLine(0, AgentLogTone.Muted, "# ${s.diagnosticNote}")
        step("branch")

        var output = ArrayList<AgentLogLine>()
        val turns = if (firstUser >= 0) s.history.drop(firstUser + 1) else s.history
        for (turn in turns) {
            output += if (turn.isUser) promptLines(turn.text, turn.author, turn.atMs) else outputLines(turn.text, turn.atMs)
        }
        val streaming = running && s.liveText.isNotBlank()
        if (streaming) output += outputLines(s.liveText, 0)
        if (output.size > MAX_OUTPUT_LINES) {
            val dropped = output.size - MAX_OUTPUT_LINES
            output = ArrayList(listOf(AgentLogLine(0, AgentLogTone.Muted, "… $dropped earlier lines")) + output.takeLast(MAX_OUTPUT_LINES))
        }
        out += output
        if (running && !streaming) out += AgentLogLine(0, AgentLogTone.Muted, "# waiting for the agent's first output…")

        val ended = if (s.completedAtMs > 0) s.completedAtMs else if (s.isActive) 0 else s.updatedAtMs
        when {
            s.isComplete -> {
                val took = duration(s.createdAtMs, ended)
                out += AgentLogLine(ended, AgentLogTone.Success, if (took.isEmpty()) "✓ agent finished" else "✓ agent finished in $took")
            }
            s.isStopped -> out += AgentLogLine(ended, AgentLogTone.Warning, "■ run stopped")
            s.isFailed -> {
                val reason = s.lastError.trim()
                out += AgentLogLine(ended, AgentLogTone.Failure, if (reason.isEmpty()) "✗ run failed" else "✗ $reason")
            }
        }
        step("pull")
        step("merge", if (chain.isMerged) s.mergedAtMs else 0)

        lines = out
        isLive = s.isActive
        isStreaming = streaming
        host = if (s.targetNode.isEmpty()) "agent" else "${s.provider.ifEmpty { "agent" }}@${s.targetNode}"
        status = when {
            chain.isMerged -> "merged"
            s.isActive -> "live"
            s.isComplete -> "exit 0"
            s.isStopped -> "stopped"
            else -> "exit 1"
        }
        startedAtMs = s.createdAtMs
        endedAtMs = ended
    }

    fun elapsed(nowMs: Long): String = duration(startedAtMs, if (endedAtMs > 0) endedAtMs else nowMs)

    companion object {
        const val MAX_OUTPUT_LINES = 400
        const val MAX_PROMPT_LINES = 6

        fun promptLines(text: String, author: String, atMs: Long): List<AgentLogLine> {
            val rows = text.trim().split('\n').filter { it.isNotEmpty() }
            if (rows.isEmpty()) return emptyList()
            val who = if (author.isEmpty()) "" else "@$author "
            val result = rows.take(MAX_PROMPT_LINES).mapIndexed { offset, row ->
                AgentLogLine(if (offset == 0) atMs else 0, AgentLogTone.Prompt, if (offset == 0) "› $who$row" else "  $row")
            }.toMutableList()
            if (rows.size > MAX_PROMPT_LINES) {
                result += AgentLogLine(0, AgentLogTone.Muted, "  … ${rows.size - MAX_PROMPT_LINES} more lines")
            }
            return result
        }

        fun outputLines(text: String, atMs: Long): List<AgentLogLine> {
            val result = ArrayList<AgentLogLine>()
            for (raw in text.split('\n')) {
                val row = raw.replace("\t", "    ")
                if (row.isBlank()) continue
                val tone = when {
                    row.startsWith("▸") -> AgentLogTone.Tool
                    row == "…" -> AgentLogTone.Muted
                    else -> AgentLogTone.Output
                }
                result += AgentLogLine(if (result.isEmpty()) atMs else 0, tone, row)
            }
            return result
        }

        fun duration(start: Long, end: Long): String {
            if (start <= 0 || end < start) return ""
            val seconds = (end - start) / 1000
            return when {
                seconds < 60 -> "${seconds}s"
                seconds < 3600 -> "${seconds / 60}m ${"%02d".format(seconds % 60)}s"
                else -> "${seconds / 3600}h ${"%02d".format((seconds % 3600) / 60)}m"
            }
        }
    }
}
