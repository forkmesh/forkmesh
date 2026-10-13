package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.DesktopControlHub
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.localrepos.GitAuthor
import com.forkmesh.nativeapp.localrepos.GitWorkspace
import com.forkmesh.nativeapp.localrepos.LocalRepoRecord
import com.forkmesh.nativeapp.localrepos.PullRequestService

interface AgentHandoffDesktop {
    val desktopName: String

    fun handoff(owner: String, name: String, branch: String, base: String, task: String, title: String): Int
}

class HubAgentHandoffDesktop(private val hub: () -> DesktopControlHub?) : AgentHandoffDesktop {
    override val desktopName: String
        get() = hub()?.desktop?.name?.takeIf { it.isNotEmpty() } ?: "your desktop"

    override fun handoff(owner: String, name: String, branch: String, base: String, task: String, title: String): Int {
        val hub = hub() ?: throw LocalControlException("not_connected")
        val params = linkedMapOf<String, Any?>(
            "owner" to owner, "name" to name, "branch" to branch, "base" to base, "task" to task, "title" to title,
        )
        return (hub.request("agents.handoff", params, 60_000)["id"] as? Number)?.toInt() ?: 0
    }
}

data class AgentDesktopMove(val agentId: Int, val desktopName: String, val branch: String) {
    val message: String get() = "Continuing on $desktopName as agent #$agentId, on $branch."
}

class AgentDesktopMoveException(override val message: String) : RuntimeException(message)

object AgentDesktopHandoff {
    const val MAX_TASK_CHARS = 7600
    private const val MAX_PROMPT_CHARS = 3000
    private const val MAX_LINE_CHARS = 600

    fun publishedRepo(record: LocalRepoRecord?): Pair<String, String>? {
        val parts = (record?.publishedAs ?: "").trim().split('/')
        if (parts.size != 2 || parts[0].isEmpty() || parts[1].isEmpty()) return null
        return parts[0] to parts[1]
    }

    fun canMove(run: AgentRunHandle?, record: LocalRepoRecord?, movedBranch: String?): Boolean =
        run != null && publishedRepo(record) != null && movedBranch != run.branch

    fun title(prompt: String): String {
        val line = prompt.lineSequence().firstOrNull()?.trim().orEmpty().ifEmpty { "Agent moved from the phone" }
        return if (line.length > 120) line.take(119) + "…" else line
    }

    fun task(prompt: String, events: List<AgentEvent>, branch: String, base: String): String {
        val trimmed = prompt.trim()
        val shown = if (trimmed.length > MAX_PROMPT_CHARS) trimmed.take(MAX_PROMPT_CHARS) + "…" else trimmed
        val head = "Continue a task that a coding agent started on my phone. Its work so far is committed on " +
            "branch $branch (started from $base), and you are working on that branch now. Check what " +
            "is already done before changing anything, then finish the task.\n\nTask:\n$shown"
        val lines = conversation(events, trimmed)
        if (lines.isEmpty()) return head
        val intro = "\n\nWhat happened on the phone:\n"
        val omitted = "… earlier steps omitted\n"
        val budget = MAX_TASK_CHARS - head.length - intro.length - omitted.length
        val kept = ArrayList<String>()
        var used = 0
        for (line in lines.asReversed()) {
            val cost = line.length + 1
            if (used + cost > budget) break
            kept.add(line)
            used += cost
        }
        if (kept.isEmpty()) return head
        kept.reverse()
        return head + intro + (if (kept.size < lines.size) omitted else "") + kept.joinToString("\n")
    }

    internal fun conversation(events: List<AgentEvent>, prompt: String): List<String> {
        val lines = ArrayList<String>()
        var skippedPrompt = false
        for (event in events) {
            when (event) {
                is AgentUserMessage -> {
                    val said = event.text.trim()
                    if (!skippedPrompt && said == prompt) {
                        skippedPrompt = true
                        continue
                    }
                    skippedPrompt = true
                    if (said.isNotEmpty()) lines.add("Me: " + clip(said))
                }
                is AgentAssistantMessage -> event.text.trim().takeIf { it.isNotEmpty() }?.let { lines.add("Agent: " + clip(it)) }
                is AgentToolCallEvent -> {
                    val target = listOf("path", "command", "pattern", "query", "branch", "message")
                        .firstNotNullOfOrNull { key -> (event.call.arguments[key] as? String)?.trim()?.takeIf { it.isNotEmpty() } }
                    lines.add("Tool: " + event.call.name + (target?.let { " " + clip(it) } ?: ""))
                }
                is AgentToolResultEvent -> if (event.result.isError) lines.add("Tool error: " + clip(event.result.output.trim()))
                is AgentNotice -> if (event.isError) lines.add("Note: " + clip(event.text.trim()))
                is AgentDone -> lines.add("Run ended: " + stopText(event.reason))
            }
        }
        return lines
    }

    private fun stopText(reason: AgentStopReason): String = when (reason) {
        AgentStopReason.Completed -> "the agent said it was done"
        AgentStopReason.Cancelled -> "stopped to move it to the desktop"
        AgentStopReason.TurnLimit -> "hit its turn limit"
        AgentStopReason.Error -> "an error"
        AgentStopReason.Refused -> "the model refused"
    }

    private fun clip(text: String): String {
        val flat = text.replace('\n', ' ')
        return if (flat.length > MAX_LINE_CHARS) flat.take(MAX_LINE_CHARS - 1) + "…" else flat
    }

    fun failureText(error: Throwable, repo: String, branch: String): String {
        if (error is LocalControlException) {
            return when (error.code) {
                "unsupported", "unknown_method" -> "Update ForkMesh on the desktop to move agents to it."
                "no_checkout" -> "The desktop has no working copy of $repo. Clone it there first, then move the agent."
                "branch_not_found" -> "The desktop could not fetch $branch from ForkMesh. The branch is pushed; try again in a moment."
                "bad_branch", "bad_task" -> "The desktop refused the move (${error.code})."
                "not_started" ->
                    if (error.reason.isBlank()) "The desktop could not start the agent. It says why in its Agents list."
                    else ControlErrorText.withReason("The desktop could not start the agent.", error)
                else -> "Could not reach your desktop. Open ForkMesh there and try again."
            }
        }
        return error.message ?: error.toString()
    }

    fun move(
        run: AgentRunHandle,
        sessions: AgentSessionService,
        workspace: GitWorkspace,
        record: LocalRepoRecord,
        pullRequests: PullRequestService,
        desktop: AgentHandoffDesktop,
        author: GitAuthor,
    ): AgentDesktopMove {
        val (owner, name) = publishedRepo(record)
            ?: throw AgentDesktopMoveException("This repository is only on this phone. Publish it to ForkMesh first.")
        if (!run.isComplete) {
            sessions.cancelActive()
            run.awaitDone()
        }
        try {
            commitLeftovers(run, workspace, author)
            val push = pullRequests.pushBranch.push(workspace, owner, name, run.branch)
            if (push.isRejected) {
                throw AgentDesktopMoveException("ForkMesh rejected the push of ${run.branch}, so the agent stayed on this phone.")
            }
        } catch (failure: AgentDesktopMoveException) {
            throw failure
        } catch (failure: Exception) {
            throw AgentDesktopMoveException("Could not push ${run.branch}: ${failure.message ?: failure}")
        }
        val id = try {
            desktop.handoff(owner, name, run.branch, run.baseBranch, task(run.task.prompt, run.events, run.branch, run.baseBranch), title(run.task.prompt))
        } catch (failure: Exception) {
            throw AgentDesktopMoveException(failureText(failure, "$owner/$name", run.branch))
        }
        return AgentDesktopMove(id, desktop.desktopName, run.branch)
    }

    private fun commitLeftovers(run: AgentRunHandle, workspace: GitWorkspace, author: GitAuthor) {
        if (workspace.status().isClean) return
        val branches = workspace.branches()
        if (branches.firstOrNull { it.isCurrent }?.name != run.branch) return
        workspace.stageAll()
        workspace.commit("Work in progress, moved to the desktop", author)
        if (branches.any { it.name == run.baseBranch }) workspace.checkout(run.baseBranch)
    }
}
