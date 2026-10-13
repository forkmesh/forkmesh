package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.api.HttpTransport

sealed class AgentEvent

data class AgentUserMessage(val text: String, val imageCount: Int = 0) : AgentEvent()

data class AgentAssistantMessage(val text: String) : AgentEvent()

data class AgentToolCallEvent(val call: AgentToolCall) : AgentEvent()

data class AgentToolResultEvent(val result: AgentToolResult) : AgentEvent()

data class AgentNotice(val text: String, val isError: Boolean = false) : AgentEvent()

enum class AgentStopReason { Completed, Cancelled, TurnLimit, Error, Refused }

data class AgentDone(val reason: AgentStopReason, val turns: Int, val inputTokens: Int, val outputTokens: Int) : AgentEvent()

open class CodingAgent(
    val provider: AgentProvider,
    val toolset: AgentToolset,
    val maxTurns: Int = 24,
    val maxHistoryMessages: Int = 60,
    private val transport: HttpTransport = HttpTransport.DEFAULT,
    private val retryDelayMs: Long = 2000,
) {
    private val history = ArrayList<Any>()
    private val queuedSteering = ArrayList<String>()
    private val queuedImages = ArrayList<PromptAttachment>()
    private val lock = Any()

    @Volatile
    var isRunning: Boolean = false
        private set

    @Volatile
    private var cancelled = false

    val hasHistory: Boolean get() = history.isNotEmpty()

    fun steer(text: String, images: List<PromptAttachment> = emptyList()) {
        val trimmed = text.trim()
        if (trimmed.isEmpty() && images.isEmpty()) return
        synchronized(lock) {
            queuedSteering.add(trimmed.ifEmpty { "See the attached photos." })
            queuedImages.addAll(images)
        }
    }

    fun cancel() {
        cancelled = true
    }

    open fun dispose() {}

    fun run(prompt: String, images: List<PromptAttachment> = emptyList(), emit: (AgentEvent) -> Unit) {
        if (isRunning) {
            emit(AgentNotice("This session is already running. Send a follow-up to steer it.", isError = true))
            return
        }
        isRunning = true
        cancelled = false
        var turns = 0
        var inputTokens = 0
        var outputTokens = 0
        fun done(reason: AgentStopReason) = emit(AgentDone(reason, turns, inputTokens, outputTokens))
        try {
            provider.appendUser(history, prompt, images)
            emit(AgentUserMessage(prompt, images.size))
            while (true) {
                if (cancelled) {
                    emit(AgentNotice("Stopped. The work so far is kept."))
                    done(AgentStopReason.Cancelled)
                    return
                }
                if (turns >= maxTurns) {
                    emit(
                        AgentNotice(
                            "Stopped after $maxTurns turns without finishing. Send a follow-up to continue from here.",
                            isError = true,
                        ),
                    )
                    done(AgentStopReason.TurnLimit)
                    return
                }
                trimHistory()
                turns++
                val turn = try {
                    sendWithOneRetry()
                } catch (error: AgentProviderException) {
                    emit(AgentNotice(error.message, isError = true))
                    done(AgentStopReason.Error)
                    return
                }
                inputTokens += turn.inputTokens
                outputTokens += turn.outputTokens
                provider.appendAssistant(history, turn)
                val refusal = turn.refusal
                if (refusal != null) {
                    emit(AgentNotice(refusal, isError = true))
                    done(AgentStopReason.Refused)
                    return
                }
                if (turn.text.trim().isNotEmpty()) emit(AgentAssistantMessage(turn.text.trim()))
                if (!turn.wantsTools) {
                    val queued = takeSteering()
                    if (queued != null) {
                        val steerImages = takeSteeringImages()
                        provider.appendUser(history, queued, steerImages)
                        emit(AgentUserMessage(queued, steerImages.size))
                        continue
                    }
                    done(AgentStopReason.Completed)
                    return
                }
                val results = ArrayList<AgentToolResult>()
                for (call in turn.toolCalls) {
                    emit(AgentToolCallEvent(call))
                    val result = if (cancelled) {
                        AgentToolResult(call.id, call.name, "Cancelled by the user before this ran.", true)
                    } else {
                        toolset.invoke(call.id, call.name, call.arguments)
                    }
                    results.add(result)
                    emit(AgentToolResultEvent(result))
                }
                provider.appendToolResults(history, results)
                val queued = takeSteering()
                if (queued != null) {
                    val steerImages = takeSteeringImages()
                    provider.appendUser(history, queued, steerImages)
                    emit(AgentUserMessage(queued, steerImages.size))
                }
            }
        } finally {
            isRunning = false
        }
    }

    private fun sendWithOneRetry(): AgentTurn = try {
        send()
    } catch (error: AgentProviderException) {
        if (!error.isRetryable) throw error
        if (retryDelayMs > 0) Thread.sleep(retryDelayMs)
        send()
    }

    private fun send(): AgentTurn = try {
        provider.send(systemPrompt, history, toolset.tools, transport)
    } catch (error: AgentProviderException) {
        throw error
    } catch (error: Exception) {
        throw AgentProviderException("Could not reach the model: ${error.message ?: error}")
    }

    private fun takeSteering(): String? = synchronized(lock) {
        if (queuedSteering.isEmpty()) return null
        val joined = queuedSteering.joinToString("\n\n")
        queuedSteering.clear()
        joined
    }

    private fun takeSteeringImages(): List<PromptAttachment> = synchronized(lock) {
        val images = ArrayList(queuedImages)
        queuedImages.clear()
        images
    }

    private fun trimHistory() {
        if (history.size <= maxHistoryMessages) return
        val surplus = history.size - maxHistoryMessages
        history.subList(1, 1 + surplus).clear()
    }

    val systemPrompt: String
        get() = "You are a coding agent running inside the ForkMesh mobile app, working in a real Git repository on the user's phone.\n\n" +
            "You have no shell. You cannot run builds, tests, linters, or any command — those tools do not exist here, and the " +
            "platform forbids them. Work only through the tools you were given.\n\n" +
            "Because you cannot verify by running anything, prefer small, careful edits: read before you write, keep changes " +
            "close to the surrounding code, and say what you could not check. If a task genuinely needs a build or a test run, " +
            "do the part you can and say plainly which part needs a desktop.\n\n" +
            "Commit with git_commit when the change is complete."
}
