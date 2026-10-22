package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.jsonText
import java.util.Locale

enum class CallQualityGrade(val wire: String, val label: String) {
    GOOD("good", "Good connection"),
    FAIR("fair", "Fair connection"),
    POOR("poor", "Poor connection"),
    UNKNOWN("unknown", "Measuring connection"),
    ;

    companion object {
        fun of(wire: String): CallQualityGrade? = entries.firstOrNull { it.wire == wire }

        fun ofBars(bars: Int): CallQualityGrade = when {
            bars <= 0 -> UNKNOWN
            bars >= 4 -> GOOD
            bars == 1 -> POOR
            else -> FAIR
        }
    }
}

enum class CallLossMark { NONE, FAIR, POOR }

data class CallLevel(val mic: Double, val peer: Double?)

data class CallQualitySample(
    val live: Boolean = false,
    val grade: CallQualityGrade = CallQualityGrade.UNKNOWN,
    val bars: Int = 0,
    val rttMs: Double? = null,
    val lossPct: Double? = null,
    val jitterMs: Double? = null,
    val sendKbps: Double? = null,
    val recvKbps: Double? = null,
    val inWidth: Int? = null,
    val inHeight: Int? = null,
    val inFps: Double? = null,
    val outWidth: Int? = null,
    val outHeight: Int? = null,
    val outFps: Double? = null,
    val limit: String = "",
)

data class CallQualityEvent(val sample: CallQualitySample, val levels: List<CallLevel>)

data class CallQualityHistory(
    val latest: CallQualitySample? = null,
    val levels: List<CallLevel> = emptyList(),
    val samples: List<CallQualitySample> = emptyList(),
) {
    val live: Boolean get() = latest?.live == true

    val hasPeer: Boolean get() = levels.any { it.peer != null }

    fun add(event: CallQualityEvent): CallQualityHistory = CallQualityHistory(
        latest = event.sample,
        levels = (levels + event.levels).takeLast(CallQuality.LEVEL_SAMPLES),
        samples = if (event.sample.live) (samples + event.sample).takeLast(CallQuality.STATS_SAMPLES) else samples,
    )
}

object CallQuality {
    const val LEVEL_SAMPLES = 50
    const val STATS_SAMPLES = 60
    const val PILL_SAMPLES = 20
    const val OPEN_KEY = "fm.call.qualityOpen"
    val RTT_STEPS = listOf(150.0, 300.0, 500.0)
    val LOSS_STEPS = listOf(1.0, 3.0, 8.0)
    val JITTER_STEPS = listOf(30.0, 60.0, 100.0)

    private val LIMIT_LABELS = mapOf(
        "bandwidth" to "limited by bandwidth",
        "cpu" to "limited by CPU",
        "other" to "limited",
    )

    private fun number(value: Any?): Double? {
        val number = when (value) {
            is Number -> value.toDouble()
            is String -> value.toDoubleOrNull()
            else -> null
        }
        return number?.takeIf { it.isFinite() }
    }

    private fun size(value: Any?): Int? = number(value)?.toInt()?.takeIf { it > 0 }

    private fun level(value: Any?): Double? = number(value)?.coerceIn(0.0, 1.0)

    private fun step(value: Double?, steps: List<Double>): Int = if (value == null) 0 else steps.count { value >= it }

    fun bars(rttMs: Double?, lossPct: Double?, jitterMs: Double?): Int {
        if (rttMs == null && lossPct == null) return 0
        val penalty = maxOf(step(rttMs, RTT_STEPS), step(lossPct, LOSS_STEPS), step(jitterMs, JITTER_STEPS))
        return maxOf(1, 4 - penalty)
    }

    fun parse(message: Map<String, Any?>): CallQualityEvent? {
        if (jsonText(message["event"]) != "quality") return null
        val live = message["live"] == true
        val rtt = number(message["rttMs"])
        val loss = number(message["lossPct"])
        val jitter = number(message["jitterMs"])
        val sentBars = number(message["bars"])?.toInt()
        val bars = if (!live) 0 else sentBars?.takeIf { it in 0..4 } ?: bars(rtt, loss, jitter)
        val grade = if (!live) CallQualityGrade.UNKNOWN else CallQualityGrade.of(jsonText(message["grade"])) ?: CallQualityGrade.ofBars(bars)
        val sample = CallQualitySample(
            live = live,
            grade = grade,
            bars = bars,
            rttMs = rtt,
            lossPct = loss,
            jitterMs = jitter,
            sendKbps = number(message["sendKbps"]),
            recvKbps = number(message["recvKbps"]),
            inWidth = size(message["inWidth"]),
            inHeight = size(message["inHeight"]),
            inFps = number(message["inFps"]),
            outWidth = size(message["outWidth"]),
            outHeight = size(message["outHeight"]),
            outFps = number(message["outFps"]),
            limit = jsonText(message["limit"]).takeIf { it != "none" }.orEmpty(),
        )
        val mic = message["mic"] as? List<*> ?: emptyList<Any?>()
        val peer = message["peer"] as? List<*> ?: emptyList<Any?>()
        val levels = mic.indices.map { index -> CallLevel(level(mic[index]) ?: 0.0, level(peer.getOrNull(index))) }
        return CallQualityEvent(sample, levels.takeLast(LEVEL_SAMPLES))
    }

    private fun fixed(value: Double, digits: Int): String = String.format(Locale.US, "%.${digits}f", value)

    fun formatKbps(kbps: Double?): String = when {
        kbps == null -> ""
        kbps >= 1000 -> fixed(kbps / 1000, 1) + " Mb/s"
        else -> "${Math.round(kbps)} kb/s"
    }

    fun rttText(sample: CallQualitySample?): String {
        val rtt = sample?.rttMs ?: return ""
        if (!sample.live) return ""
        return "${Math.round(rtt)} ms"
    }

    fun describeNetwork(sample: CallQualitySample?): String {
        val rtt = sample?.rttMs ?: return "Measuring…"
        val parts = mutableListOf("${Math.round(rtt)} ms")
        sample.lossPct?.let { parts.add(fixed(it, if (it < 10) 1 else 0) + "% loss") }
        sample.jitterMs?.let { parts.add("${Math.round(it)} ms jitter") }
        return parts.joinToString(" · ")
    }

    fun describeVideo(sample: CallQualitySample?, incoming: Boolean): String {
        if (sample == null) return ""
        val width = if (incoming) sample.inWidth else sample.outWidth
        val height = if (incoming) sample.inHeight else sample.outHeight
        val fps = if (incoming) sample.inFps else sample.outFps
        val kbps = if (incoming) sample.recvKbps else sample.sendKbps
        val parts = mutableListOf<String>()
        if (width != null && height != null) parts.add("$width×$height")
        if (fps != null) parts.add("${Math.round(fps)} fps")
        formatKbps(kbps).takeIf { it.isNotEmpty() }?.let(parts::add)
        if (!incoming && sample.limit.isNotEmpty()) parts.add(LIMIT_LABELS[sample.limit] ?: "limited")
        return parts.joinToString(" · ")
    }

    fun videoInText(sample: CallQualitySample?): String = describeVideo(sample, incoming = true).ifEmpty { "No video" }

    fun videoOutText(sample: CallQualitySample?): String = describeVideo(sample, incoming = false).ifEmpty { "Camera off" }

    fun heading(history: CallQualityHistory): String = history.latest?.takeIf { it.live }?.grade?.label ?: "Microphone"

    fun voiceText(history: CallQualityHistory, peerLabel: String = "Them"): String =
        if (history.hasPeer) "You ▲ · $peerLabel ▼" else "You"

    fun summary(history: CallQualityHistory): String {
        val latest = history.latest
        if (latest == null || !latest.live) return "Microphone level"
        if (latest.rttMs == null) return latest.grade.label
        return latest.grade.label + " · " + describeNetwork(latest)
    }

    fun accessibility(history: CallQualityHistory): String = "Call quality: " + summary(history)

    fun lossMark(lossPct: Double?): CallLossMark = when {
        lossPct == null || lossPct < LOSS_STEPS[0] -> CallLossMark.NONE
        lossPct >= LOSS_STEPS[2] -> CallLossMark.POOR
        else -> CallLossMark.FAIR
    }
}
