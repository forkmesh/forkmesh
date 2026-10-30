package com.forkmesh.nativeapp.composer

import kotlin.math.max

sealed class VoiceLaunchPhase {
    object Closed : VoiceLaunchPhase()
    object Waking : VoiceLaunchPhase()
    object Listening : VoiceLaunchPhase()
    object Sending : VoiceLaunchPhase()
    object Sent : VoiceLaunchPhase()
    object Leaving : VoiceLaunchPhase()
    data class Missed(val message: String) : VoiceLaunchPhase()

    val isOpen: Boolean get() = this !is Closed

    val away: Boolean get() = this is Leaving || this is Sent
}

enum class VoiceRelease { Ignore, KeepListening, Send }

object VoiceLaunchRules {
    const val HOLD_MS = 350L
    const val SLOP_DP = 10f
    const val PAUSE_SECONDS = 3
    const val LIMIT_SECONDS = 120
    const val TAIL_MS = 250L
    const val FINISH_WAIT_MS = 2000L
    const val SENT_HOLD_MS = 800L
    const val LEAVE_HOLD_MS = 420L
    const val MISS_HOLD_MS = 2200L
    const val NOTHING_HEARD = "I didn't hear anything. Hold the button and try again."

    fun release(phase: VoiceLaunchPhase, held: Boolean, heard: String, voicing: Boolean): VoiceRelease = when {
        !held || phase !is VoiceLaunchPhase.Listening -> VoiceRelease.Ignore
        heard.isBlank() && !voicing -> VoiceRelease.KeepListening
        else -> VoiceRelease.Send
    }

    fun quietFill(quietMs: Long, loud: Boolean): Float {
        if (loud) return 0f
        val quiet = quietMs.toFloat() / (PAUSE_SECONDS * 1000f)
        return if (quiet > 0.08f) quiet.coerceAtMost(1f) else 0f
    }

    fun hint(phase: VoiceLaunchPhase, held: Boolean, heard: Boolean): String? = when {
        phase is VoiceLaunchPhase.Listening && held -> if (heard) "Let go to send" else "Say what the agent should do · let go to send"
        phase is VoiceLaunchPhase.Listening -> if (heard) "Pause to send · tap the orb to send now" else "Say what the agent should do"
        phase is VoiceLaunchPhase.Sending -> "Starting your agent…"
        phase is VoiceLaunchPhase.Sent -> "On its way"
        else -> null
    }

    fun placeholder(phase: VoiceLaunchPhase): String = if (phase is VoiceLaunchPhase.Waking) "Get ready…" else "Listening…"

    fun orbLabel(phase: VoiceLaunchPhase, heard: Boolean): String = when (phase) {
        is VoiceLaunchPhase.Listening -> if (heard) "Send now" else "Listening"
        is VoiceLaunchPhase.Sending -> "Starting the agent"
        is VoiceLaunchPhase.Sent -> "Agent started"
        else -> "Voice prompt"
    }

    fun symbol(phase: VoiceLaunchPhase): String = when (phase) {
        is VoiceLaunchPhase.Waking -> "auto_awesome"
        is VoiceLaunchPhase.Listening -> "graphic_eq"
        is VoiceLaunchPhase.Sending -> "arrow_upward_rounded"
        is VoiceLaunchPhase.Sent -> "check"
        is VoiceLaunchPhase.Missed -> "mic_off"
        else -> "close"
    }

    fun canCancel(phase: VoiceLaunchPhase): Boolean = phase.isOpen && phase !is VoiceLaunchPhase.Leaving &&
        phase !is VoiceLaunchPhase.Sent && phase !is VoiceLaunchPhase.Sending

    fun typed(base: String, heard: String): String = DictationText.merge(base, heard.trim())
}

class VoiceTranscript(private val limit: Int = com.forkmesh.nativeapp.agents.AgentQueueApi.MAX_AGENT_PROMPT_CHARS) {
    private var committed = ""
    private var current = ""

    val text: String get() = ComposerRules.clamp(DictationText.merge(committed, current).trim(), limit)

    fun partial(words: String): Boolean {
        if (words == current) return false
        current = words.trim()
        return true
    }

    fun commit() {
        committed = DictationText.merge(committed, current)
        current = ""
    }

    fun clear() {
        committed = ""
        current = ""
    }
}

class VoiceMeter(val size: Int = SIZE) {
    private val ring = FloatArray(size)
    private var next = 0

    val levels: FloatArray
        get() = FloatArray(size) { ring[(next + it) % size] }

    val loudness: Float get() = max(ring[(next + size - 1) % size], max(ring[(next + size - 2) % size], ring[(next + size - 3) % size]))

    val voicing: Boolean get() = loudness >= VOICE

    fun push(rmsDb: Float) {
        val raw = level(rmsDb)
        val previous = ring[(next + size - 1) % size]
        ring[next] = if (raw >= previous) raw else previous * 0.65f + raw * 0.35f
        next = (next + 1) % size
    }

    fun clear() {
        ring.fill(0f)
        next = 0
    }

    companion object {
        const val SIZE = 24
        const val VOICE = 0.3f
        const val FLOOR_DB = -2f
        const val RANGE_DB = 12f

        fun level(rmsDb: Float): Float = ((rmsDb - FLOOR_DB) / RANGE_DB).coerceIn(0f, 1f)
    }
}
