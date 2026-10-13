package com.forkmesh.nativeapp.agentwatch

import com.forkmesh.nativeapp.agents.AgentAssistantMessage
import com.forkmesh.nativeapp.agents.AgentDone
import com.forkmesh.nativeapp.agents.AgentEvent
import com.forkmesh.nativeapp.agents.AgentNotice
import com.forkmesh.nativeapp.agents.AgentSessionService
import com.forkmesh.nativeapp.agents.AgentStopReason
import com.forkmesh.nativeapp.agents.AgentToolCallEvent
import com.forkmesh.nativeapp.agents.AgentToolResultEvent
import com.forkmesh.nativeapp.agents.AgentUserMessage
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.control.AgentControlling
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.Repository

class AgentWatchSealedException : IllegalStateException("Sealed for another owner device.")

fun relayWatchTarget(api: ApiService, repo: Repository, session: AgentSession, ownerAccount: String): AgentWatchTarget =
    AgentWatchTarget(
        key = "relay:${repo.fullName}#${session.id}",
        id = session.id,
        title = session.displayTitle,
        subtitle = repo.fullName,
        project = repo.name,
        provider = session.provider,
        model = session.model,
        turns = session.numTurns,
        tokens = session.totalTokens,
        load = {
            val detail = api.agentTranscript(repo.owner, repo.name, session.id, ownerAccount)
            if (detail.sealedForOtherDevice) throw AgentWatchSealedException()
            val status = detail.status.ifEmpty { session.status }
            AgentWatchSnapshot(if (session.merged) "done" else status, beatsFromTranscript(detail.transcript))
        },
    )

fun controlWatchTarget(control: AgentControlling, row: UnifiedAgentRow, feeds: ControlWatchFeeds? = null): AgentWatchTarget {
    val number = if (row.localId > 0) row.localId else 0
    return AgentWatchTarget(
        key = "control:${row.id}",
        id = number,
        title = row.title,
        subtitle = row.repoLabel.ifEmpty { row.cwd },
        project = row.repoName.ifEmpty { row.cwd.trimEnd('/').substringAfterLast('/') },
        provider = row.provider,
        model = row.model,
        load = {
            val current = control.rows.firstOrNull { it.id == row.id }
            val streamed = feeds?.text(row.id)
            if (streamed != null) {
                AgentWatchSnapshot(streamed.status.ifEmpty { current?.status ?: row.status }, beatsFromTranscript(streamed.text))
            } else {
                val delta = control.transcript(row, 0)
                AgentWatchSnapshot(delta.status.ifEmpty { current?.status ?: row.status }, beatsFromTranscript(delta.text))
            }
        },
    )
}

fun deviceWatchTarget(sessions: AgentSessionService, repoName: String): AgentWatchTarget {
    val prompt = sessions.lastRun?.task?.prompt?.trim() ?: ""
    val firstLine = prompt.split("\n").first().trim()
    val slash = repoName.lastIndexOf('/')
    return AgentWatchTarget(
        key = "device:$repoName",
        id = 0,
        title = firstLine.ifEmpty { repoName },
        subtitle = "On this phone",
        project = if (slash >= 0) repoName.substring(slash + 1) else repoName,
        prompt = prompt,
        load = { deviceSnapshot(sessions) },
    )
}

fun deviceSnapshot(sessions: AgentSessionService): AgentWatchSnapshot {
    val run = sessions.lastRun
    val events = run?.events ?: emptyList()
    val running = run != null && !run.isComplete
    val failed = events.any { it is AgentDone && (it.reason == AgentStopReason.Error || it.reason == AgentStopReason.Refused) }
    return AgentWatchSnapshot(if (running) "running" else if (failed) "failed" else "done", beatsFromAgentEvents(events))
}

private val refusals = listOf("denied", "permission", "rejected", "doesn't want", "not allowed", "blocked", "refused")

private fun resultMotion(isError: Boolean, output: String): AgentMotion {
    if (!isError) return AgentMotion.arrive
    val head = if (output.length > 2000) output.substring(0, 2000) else output
    val lower = head.lowercase()
    return if (refusals.any { lower.contains(it) }) AgentMotion.denied else AgentMotion.fumble
}

private fun argumentText(value: Any?): String? = when (value) {
    null -> null
    is String -> value
    else -> value.toString()
}

fun beatsFromAgentEvents(events: List<AgentEvent>): List<WatchBeat> {
    val beats = ArrayList<WatchBeat>()
    for (event in events) {
        when (event) {
            is AgentUserMessage -> beats += WatchBeat(AgentMotion.prompt, clip(event.text))
            is AgentAssistantMessage -> if (event.text.trim().isNotEmpty()) {
                beats += WatchBeat(AgentMotion.speak, clip(event.text), event.text.length, plainProse(event.text))
            }
            is AgentToolCallEvent -> {
                val command = argumentText(event.call.arguments["command"])
                val path = argumentText(event.call.arguments["path"]) ?: ""
                beats += WatchBeat(motionForTool(event.call.name, command ?: ""), clip("${event.call.name} ${command ?: path}"))
            }
            is AgentToolResultEvent -> beats += WatchBeat(
                resultMotion(event.result.isError, event.result.output),
                clip("${event.result.name}: ${event.result.output}"),
            )
            is AgentNotice -> beats += WatchBeat(if (event.isError) AgentMotion.notice else AgentMotion.system, clip(event.text))
            is AgentDone -> {
                val failed = event.reason == AgentStopReason.Error || event.reason == AgentStopReason.Refused
                val reason = event.reason.name.replaceFirstChar { it.lowercase() }
                beats += WatchBeat(
                    if (failed) AgentMotion.fail else AgentMotion.finish,
                    "$reason after ${event.turns} ${if (event.turns == 1) "turn" else "turns"}",
                )
            }
        }
    }
    return beats
}
