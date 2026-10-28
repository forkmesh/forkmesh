package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.UnifiedAgentRow
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

enum class AgentTokenPace {
    Idle,
    Slow,
    Steady,
    Fast,
    Blazing,
    ;

    companion object {
        const val INPUT_SCALE = 10.0
        const val MAX_TRAVEL = 24.0

        fun of(perSecond: Double, input: Boolean): AgentTokenPace {
            val rate = if (input) perSecond / INPUT_SCALE else perSecond
            return when {
                rate < 1 -> Idle
                rate < 15 -> Slow
                rate < 60 -> Steady
                rate < 200 -> Fast
                else -> Blazing
            }
        }

        fun travel(perSecond: Double, input: Boolean): Double {
            val rate = if (input) perSecond / INPUT_SCALE else perSecond
            if (rate < 1) return 0.0
            return min(MAX_TRAVEL, 4 * (1 + log10(rate)))
        }
    }
}

class AgentTokenSampler {
    companion object {
        const val LONGEST_GAP_MS = 8_000L
        const val HALF_LIFE_SECONDS = 1.5
        const val FLOOR_PER_SECOND = 0.5
        const val SETTLE_AFTER_MS = 10_000L
        const val HOLD_EVERY_MS = 1_000L

        private fun blend(old: Double, grown: Long, seconds: Double): Double {
            if (grown < 0) return 0.0
            if (grown == 0L) {
                val faded = old * 0.5.pow(seconds / HALF_LIFE_SECONDS)
                return if (faded < FLOOR_PER_SECOND) 0.0 else faded
            }
            val fresh = grown / seconds
            return if (old == 0.0) fresh else (old + fresh) / 2
        }
    }

    var inPerSecond = 0.0
        private set
    var outPerSecond = 0.0
        private set
    private var lastIn = -1L
    private var lastOut = -1L
    private var lastMs = 0L

    val inPace: AgentTokenPace get() = AgentTokenPace.of(inPerSecond, input = true)
    val outPace: AgentTokenPace get() = AgentTokenPace.of(outPerSecond, input = false)
    val inTravel: Double get() = AgentTokenPace.travel(inPerSecond, input = true)
    val outTravel: Double get() = AgentTokenPace.travel(outPerSecond, input = false)

    fun sample(tokensIn: Long, tokensOut: Long, atMs: Long) {
        if (lastIn >= 0 && atMs > lastMs) {
            val seconds = min(atMs - lastMs, LONGEST_GAP_MS) / 1000.0
            inPerSecond = blend(inPerSecond, tokensIn - lastIn, seconds)
            outPerSecond = blend(outPerSecond, tokensOut - lastOut, seconds)
        }
        lastIn = tokensIn
        lastOut = tokensOut
        lastMs = atMs
    }

    fun hold(atMs: Long) {
        if (lastIn < 0) return
        sample(lastIn, lastOut, atMs)
    }

    fun settle() {
        inPerSecond = 0.0
        outPerSecond = 0.0
    }
}

class AgentTokenClock {
    private var last: Double? = null
    var inPhase = 0.0
        private set
    var outPhase = 0.0
        private set

    fun advance(nowSeconds: Double, running: Boolean, inTravel: Double, outTravel: Double, wrap: Double) {
        val before = last
        if (running && before != null && wrap > 0) {
            val elapsed = min(max(nowSeconds - before, 0.0), 0.25)
            inPhase = (inPhase - elapsed * inTravel) % wrap
            outPhase = (outPhase + elapsed * outTravel) % wrap
        }
        last = if (running) nowSeconds else null
    }
}

class AgentTokenSeries {
    companion object {
        const val SECONDS = 60
        const val DESKTOP_KEY = "desktop"

        fun totals(desktop: List<Long>, rows: List<UnifiedAgentRow>): Map<String, List<Long>> {
            val fed = desktop.size == 2
            val out = LinkedHashMap<String, List<Long>>()
            if (fed) out[DESKTOP_KEY] = desktop
            for (row in rows) {
                if (fed && row.isDesktop) continue
                out["row:${row.id}"] = listOf(row.tokensIn.toLong(), row.tokensOut.toLong())
            }
            return out
        }

        fun summary(series: AgentTokenSeries): String {
            fun latest(values: List<Double>): String = values.lastOrNull()?.let { "${Math.round(it)} a second" } ?: "no reading"
            return "Agent tokens in ${latest(series.input)}, out ${latest(series.output)}"
        }
    }

    var input: List<Double> = emptyList()
        private set
    var output: List<Double> = emptyList()
        private set
    private var last: Map<String, List<Long>> = emptyMap()

    fun tick(totals: Map<String, List<Long>>) {
        var grownIn = 0L
        var grownOut = 0L
        for ((key, now) in totals) {
            if (now.size != 2) continue
            val before = last[key] ?: continue
            if (before.size != 2) continue
            grownIn += max(0L, now[0] - before[0])
            grownOut += max(0L, now[1] - before[1])
        }
        last = totals
        input = (input + grownIn.toDouble()).takeLast(SECONDS)
        output = (output + grownOut.toDouble()).takeLast(SECONDS)
    }
}
