package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.AgentLaunchSession

data class RunnerDesktopMove(
    val agentId: Int,
    val desktopName: String,
    val branch: String,
    val fresh: Boolean = false,
    val already: Boolean = false,
    val runnerKept: Boolean = false,
) {
    val message: String
        get() = if (runnerKept) "$moved Its run on the runner could not be deleted; delete it from Agents." else moved

    private val moved: String
        get() = when {
            already -> "Already continuing on $desktopName as agent #$agentId."
            fresh -> "Continuing on $desktopName as agent #$agentId, from the base branch: the runner's work was not on ForkMesh."
            branch.isEmpty() -> "Continuing on $desktopName as agent #$agentId."
            else -> "Continuing on $desktopName as agent #$agentId, on $branch."
        }
}

class RunnerDesktopMoveException(override val message: String) : RuntimeException(message)

object RunnerDesktopHandoff {
    const val CAPABILITY = "agents.takeover"
    const val MAX_TASK_CHARS = 7600
    private const val MAX_PROMPT_CHARS = 3000
    private const val MAX_TURN_CHARS = 1200
    private const val MAX_REASON_CHARS = 600

    fun canMove(row: UnifiedAgentRow, desktopPaired: Boolean): Boolean =
        desktopPaired && !row.isDesktop && !row.outside && row.cloudSessionId.isNotEmpty() && row.repoName.isNotEmpty() && !row.merged

    fun runnerName(session: AgentLaunchSession): String =
        session.runner.trim().ifEmpty { session.targetNode.trim().ifEmpty { "the runner" } }

    fun params(session: AgentLaunchSession, row: UnifiedAgentRow, check: Boolean): Map<String, Any?> {
        val params = linkedMapOf<String, Any?>(
            "cloudSession" to row.cloudSessionId,
            "owner" to session.owner.ifEmpty { row.repoOwner },
            "name" to session.repo.ifEmpty { row.repoName },
            "branch" to session.branchName.trim().ifEmpty { row.branchName },
            "base" to session.baseBranch.trim().ifEmpty { row.baseBranch },
            "runner" to runnerName(session),
            "task" to task(session),
            "title" to title(session),
        )
        session.provider.ifEmpty { row.provider }.takeIf { it.isNotEmpty() }?.let { params["provider"] = it }
        session.requestedModel.ifEmpty { row.model }.takeIf { it.isNotEmpty() }?.let { params["model"] = it }
        if (check) params["check"] = true
        return params
    }

    fun title(session: AgentLaunchSession): String {
        val line = session.title.trim().ifEmpty { session.retryPrompt.lineSequence().firstOrNull()?.trim().orEmpty() }
        return if (line.isEmpty()) "Continued runner run" else clip("Continue: $line", 120)
    }

    fun task(session: AgentLaunchSession): String {
        val branch = session.branchName.trim()
        val base = session.baseBranch.trim()
        val head = StringBuilder("Continue a task that the ForkMesh runner ${runnerName(session)} started but did not finish")
        head.append(" (session ${session.sessionId}, it ended as \"${session.status.trim()}\").")
        val reason = session.diagnostic.trim().ifEmpty { session.lastError.trim() }
        if (reason.isNotEmpty()) head.append(" It stopped with: ").append(clip(flat(reason), MAX_REASON_CHARS))
        if (branch.isEmpty()) {
            head.append(" It left no branch, so start from the base branch.")
        } else {
            head.append(" Its work so far was pushed to branch $branch")
            head.append(if (base.isEmpty()) "." else " (started from $base).")
            head.append(" If you are on that branch, check what is already done before changing anything.")
        }
        val prompt = session.retryPrompt
        head.append(" Then finish the task.\n\nTask:\n").append(if (prompt.isEmpty()) "(not recorded)" else clip(prompt, MAX_PROMPT_CHARS))
        val text = head.toString()
        val lines = conversation(session)
        if (lines.isEmpty()) return text
        val intro = "\n\nWhat happened on the runner:\n"
        val omitted = "… earlier steps omitted\n"
        val budget = MAX_TASK_CHARS - text.length - intro.length - omitted.length
        val kept = ArrayList<String>()
        var used = 0
        for (line in lines.asReversed()) {
            val cost = line.length + 1
            if (used + cost > budget) break
            kept.add(line)
            used += cost
        }
        if (kept.isEmpty()) return text
        kept.reverse()
        return text + intro + (if (kept.size < lines.size) omitted else "") + kept.joinToString("\n")
    }

    internal fun conversation(session: AgentLaunchSession): List<String> {
        val lines = ArrayList<String>()
        var skippedPrompt = false
        for (turn in session.history) {
            val said = turn.text.trim()
            if (said.isEmpty()) continue
            if (turn.isUser && !skippedPrompt) {
                skippedPrompt = true
                continue
            }
            val who = when {
                turn.isUser -> "Me"
                turn.role.lowercase() == "security" -> "Note"
                else -> "Agent"
            }
            lines.add("$who: " + clip(flat(said), MAX_TURN_CHARS))
        }
        return lines
    }

    fun failureText(error: Throwable, repo: String, stoppedOn: String = ""): String {
        val after = if (stoppedOn.isEmpty()) "" else " The run on $stoppedOn is stopped; Continue restarts it there."
        if (error is RunnerDesktopMoveException) return error.message
        if (error is LocalControlException) {
            return when (error.code) {
                "unsupported", "unknown_method" -> "Update ForkMesh on the desktop to move runner agents to it."
                "no_checkout" -> "The desktop has no working copy of $repo. Clone it there first, then move the agent."
                "bad_branch", "bad_task", "bad_session" -> "The desktop refused the move (${error.code})."
                "not_started" ->
                    if (error.reason.isBlank()) "The desktop could not start the agent. It says why in its Agents list."
                    else ControlErrorText.withReason("The desktop could not start the agent.", error)
                else -> ControlErrorText.describe(error)
            } + after
        }
        return (error.message ?: error.toString()) + after
    }

    private fun flat(text: String): String = text.replace("\r\n", "\n").replace('\n', ' ')

    private fun clip(text: String, limit: Int): String {
        if (text.length <= limit) return text
        var end = limit - 1
        if (end > 0 && Character.isHighSurrogate(text[end - 1])) end--
        return text.substring(0, end) + "…"
    }
}
