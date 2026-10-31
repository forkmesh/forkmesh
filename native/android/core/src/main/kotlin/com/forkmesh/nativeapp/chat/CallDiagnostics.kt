package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonText
import java.math.BigDecimal
import java.math.RoundingMode
import kotlin.math.abs
import kotlin.math.floor

enum class CallDiagnosisSeverity(val wire: String) {
    OK("ok"),
    WARN("warn"),
    BAD("bad"),
    UNKNOWN("unknown"),
}

data class CallDiagnosisFix(val id: String, val text: String, val worked: Int = 0, val failed: Int = 0)

data class CallDiagnosisIssue(
    val id: String,
    val severity: CallDiagnosisSeverity,
    val title: String,
    val area: String,
    val cause: String,
    val detail: String,
    val fixes: List<CallDiagnosisFix>,
)

data class CallDiagnosisMetrics(
    val samples: Int = 0,
    val rttMs: Double? = null,
    val lossPct: Double? = null,
    val jitterMs: Double? = null,
    val sendKbps: Double? = null,
    val recvKbps: Double? = null,
    val outFps: Double? = null,
    val outHeight: Double? = null,
    val inFps: Double? = null,
    val inHeight: Double? = null,
    val faceMs: Double? = null,
    val faceFps: Double? = null,
    val faceLevel: Double? = null,
) {
    val entries: List<Pair<String, Double?>>
        get() = listOf(
            "rttMs" to rttMs, "lossPct" to lossPct, "jitterMs" to jitterMs, "sendKbps" to sendKbps,
            "recvKbps" to recvKbps, "outFps" to outFps, "outHeight" to outHeight, "inFps" to inFps,
            "inHeight" to inHeight, "faceMs" to faceMs, "faceFps" to faceFps, "faceLevel" to faceLevel,
        )
}

data class CallDiagnosis(
    val at: Double?,
    val severity: CallDiagnosisSeverity,
    val issues: List<CallDiagnosisIssue>,
    val metrics: CallDiagnosisMetrics,
) {
    companion object {
        val EMPTY: CallDiagnosis by lazy { CallDiagnostics.diagnose(emptyList()) }
    }

    val problem: Boolean get() = severity == CallDiagnosisSeverity.WARN || severity == CallDiagnosisSeverity.BAD

    val summary: String
        get() {
            val count = issues.size
            if (count > 0) {
                val names = issues.joinToString(", ") { it.title.lowercase() }
                return "$count problem${if (count == 1) "" else "s"} found: $names"
            }
            return if (severity == CallDiagnosisSeverity.OK) "No problems found" else "Checking the call"
        }

    val heading: String
        get() {
            val count = issues.size
            if (count > 0) return "$count problem${if (count == 1) "" else "s"} found"
            return if (severity == CallDiagnosisSeverity.OK) "No problems found" else "Not enough measurements yet"
        }
}

data class CallDiagnosticsReport(val at: Double, val issues: List<String>, val metrics: Map<String, Any?>)

data class CallFixRecord(val worked: Int = 0, val failed: Int = 0)

data class CallDiagnosticsMemory(
    val reports: MutableList<CallDiagnosticsReport> = mutableListOf(),
    val fixes: MutableMap<String, CallFixRecord> = linkedMapOf(),
    val notes: MutableList<String> = mutableListOf(),
) {
    companion object {
        fun parse(text: String?): CallDiagnosticsMemory {
            val memory = CallDiagnosticsMemory()
            if (text == null) return memory
            val raw = runCatching { Json.parse(text) }.getOrNull() as? Map<*, *> ?: return memory
            (raw["reports"] as? List<*>)?.takeLast(CallDiagnostics.REPORTS)?.forEach { value ->
                val entry = value.asJsonMap()
                memory.reports.add(
                    CallDiagnosticsReport(
                        at = (entry["at"] as? Number)?.toDouble() ?: 0.0,
                        issues = (entry["issues"] as? List<*>).orEmpty().map(::jsonText),
                        metrics = LinkedHashMap(entry["metrics"].asJsonMap()),
                    ),
                )
            }
            (raw["fixes"] as? Map<*, *>)?.forEach { (key, value) ->
                val entry = value.asJsonMap()
                memory.fixes[key.toString()] = CallFixRecord(
                    worked = ((entry["worked"] as? Number)?.toDouble() ?: 0.0).toInt(),
                    failed = ((entry["failed"] as? Number)?.toDouble() ?: 0.0).toInt(),
                )
            }
            (raw["notes"] as? List<*>)?.filterIsInstance<String>()?.takeLast(CallDiagnostics.NOTES)?.let(memory.notes::addAll)
            return memory
        }
    }

    val json: String
        get() {
            val records = LinkedHashMap<String, Any?>()
            for (key in fixes.keys.sorted()) {
                val record = fixes[key] ?: CallFixRecord()
                records[key] = linkedMapOf("worked" to record.worked.toLong(), "failed" to record.failed.toLong())
            }
            val list = reports.map { report ->
                linkedMapOf("at" to CallDiagnostics.number(report.at), "issues" to report.issues, "metrics" to report.metrics)
            }
            return Json.stringify(linkedMapOf("reports" to list, "fixes" to records, "notes" to notes))
        }

    fun record(issue: String, fix: String): CallFixRecord = fixes["$issue:$fix"] ?: CallFixRecord()

    fun clone(): CallDiagnosticsMemory = CallDiagnosticsMemory(
        reports.toMutableList(),
        LinkedHashMap(fixes),
        notes.toMutableList(),
    )
}

data class CallDiagnosticsEnvironment(
    val userAgent: String = "",
    val cores: Int = 0,
    val memory: Double = 0.0,
    val connection: String = "",
)

object CallDiagnostics {
    const val KEY = "fm.call.diagnostics"
    const val WINDOW = 8
    const val MIN_SAMPLES = 3
    const val REPORTS = 20
    const val NOTES = 8
    const val NOTE_CHARS = 280
    const val VIDEO_MIN_FPS = 15.0
    const val VIDEO_MIN_HEIGHT = 360.0
    const val VIDEO_MIN_KBPS = 100.0
    const val FACE_SLOW_MS = 34.0
    const val FACE_BAD_MS = 50.0

    val FIXES: Map<String, String> = linkedMapOf(
        "wired" to "Use a wired connection, or move closer to the Wi-Fi router.",
        "traffic" to "Pause downloads, uploads, cloud backups and video streaming on this network.",
        "vpn" to "Turn off a VPN or proxy for the call.",
        "relay" to "If the call says Relayed, this network blocks direct connections: try another network, or allow UDP on this one.",
        "band" to "Use 5 GHz Wi-Fi instead of 2.4 GHz, away from microwaves and Bluetooth speakers.",
        "router" to "Restart the Wi-Fi router.",
        "camera" to "Turn your camera off for a minute so your voice gets the whole connection.",
        "tabs" to "Close other tabs and apps, especially other calls, games and screen recorders.",
        "filters" to "Turn off the face, background or outfit filter.",
        "power" to "Plug the device in and turn off battery saver.",
        "hardware" to "Turn on hardware acceleration in the browser's settings and restart the browser.",
        "light" to "Add light in front of you: cameras lower their frame rate in the dark.",
        "cameraApps" to "Close other apps that use the camera.",
        "theirSide" to "Ask them to press their diagnostics button: poor incoming video is usually their connection or device.",
        "background" to "Choose None or Blur as the background instead of a picture.",
        "faceOff" to "Turn the face off when the call needs to be smooth.",
    )

    class Kind(val title: String, val area: String, val cause: String, val fixes: List<String>)

    val KINDS: Map<String, Kind> = linkedMapOf(
        "latency" to Kind(
            "High latency", "network",
            "Sound and video take a long time to arrive, so you talk over each other.",
            listOf("wired", "traffic", "vpn", "relay"),
        ),
        "loss" to Kind(
            "Packet loss", "network",
            "Parts of the call are lost on the way, so voice drops out and video freezes or blurs.",
            listOf("wired", "band", "traffic", "router"),
        ),
        "jitter" to Kind(
            "Unsteady connection", "network",
            "Packets arrive unevenly, so the call buffers more and voice can sound choppy.",
            listOf("traffic", "wired", "band", "vpn"),
        ),
        "bandwidth" to Kind(
            "Upload too slow for your video", "encoder",
            "The browser is lowering your video quality because the upload cannot keep up.",
            listOf("traffic", "wired", "camera", "filters"),
        ),
        "cpu" to Kind(
            "Device too busy", "encoder",
            "The browser is lowering your video quality because the processor cannot encode it in time.",
            listOf("tabs", "filters", "power", "hardware"),
        ),
        "sendFps" to Kind(
            "Your video is choppy", "camera", "Your video leaves this device at a low frame rate.",
            listOf("light", "cameraApps", "filters", "power"),
        ),
        "receive" to Kind(
            "Their video is poor", "network",
            "The other side's video arrives small or at a low frame rate.", listOf("theirSide", "traffic", "wired"),
        ),
        "face" to Kind(
            "Face filter is slow", "face filter",
            "The face filter needs too long for each frame, so your face lags behind you.",
            listOf("tabs", "power", "hardware", "background", "faceOff"),
        ),
    )

    private fun median(values: List<Double>): Double? {
        if (values.isEmpty()) return null
        val sorted = values.sorted()
        val middle = sorted.size shr 1
        return if (sorted.size % 2 == 1) sorted[middle] else (sorted[middle - 1] + sorted[middle]) / 2
    }

    private fun mean(values: List<Double>): Double? = if (values.isEmpty()) null else values.sum() / values.size

    fun round(value: Double): Int {
        val down = floor(value)
        return (if (value - down >= 0.5) down + 1 else down).toInt()
    }

    fun fixed(value: Double, digits: Int): String {
        if (!value.isFinite()) return if (value.isNaN()) "NaN" else if (value < 0) "-Infinity" else "Infinity"
        if (value < 0) {
            val positive = fixed(-value, digits)
            return "-$positive"
        }
        return BigDecimal(value).setScale(digits, RoundingMode.HALF_UP).toPlainString()
    }

    fun text(value: Double): String {
        val formatted = Json.formatDouble(value)
        return if (formatted.endsWith(".0")) formatted.dropLast(2) else formatted
    }

    fun number(value: Double): Any = if (Math.rint(value) == value && abs(value) < 9e15) value.toLong() else value

    fun formatKbps(kbps: Double?): String {
        if (kbps == null) return ""
        if (kbps >= 1000) return fixed(kbps / 1000, 1) + " Mb/s"
        return "${round(kbps)} kb/s"
    }

    private fun describeOut(sample: CallQualitySample, fps: Double?, kbps: Double?): String {
        val parts = mutableListOf<String>()
        val width = sample.outWidth
        val height = sample.outHeight
        if (width != null && height != null && width != 0 && height != 0) parts.add("$width×$height")
        if (fps != null) parts.add("${round(fps)} fps")
        val speed = formatKbps(kbps)
        if (speed.isNotEmpty()) parts.add(speed)
        return parts.joinToString(" · ")
    }

    private fun make(id: String, severity: CallDiagnosisSeverity, detail: String): CallDiagnosisIssue {
        val kind = KINDS[id] ?: Kind(id, "", "", emptyList())
        return CallDiagnosisIssue(
            id = id,
            severity = severity,
            title = kind.title,
            area = kind.area,
            cause = kind.cause,
            detail = detail,
            fixes = kind.fixes.map { CallDiagnosisFix(it, FIXES[it].orEmpty()) },
        )
    }

    fun diagnose(
        samples: List<CallQualitySample>,
        video: Boolean = false,
        sending: Boolean = true,
        window: Int = WINDOW,
    ): CallDiagnosis {
        val last = samples.takeLast(maxOf(0, window))
        fun pick(key: (CallQualitySample) -> Double?): List<Double> = last.mapNotNull(key).filter { it.isFinite() }
        val outHeights = last.mapNotNull { it.outHeight?.toDouble() }
        val inHeights = last.mapNotNull { it.inHeight?.toDouble() }
        val need = minOf(MIN_SAMPLES, window)
        val metrics = CallDiagnosisMetrics(
            samples = last.size,
            rttMs = median(pick { it.rttMs }),
            lossPct = mean(pick { it.lossPct }),
            jitterMs = median(pick { it.jitterMs }),
            sendKbps = median(pick { it.sendKbps }),
            recvKbps = median(pick { it.recvKbps }),
            outFps = median(pick { it.outFps }),
            outHeight = median(outHeights),
            inFps = median(pick { it.inFps }),
            inHeight = median(inHeights),
            faceMs = median(pick { it.faceMs }),
            faceFps = median(pick { it.faceFps }),
            faceLevel = last.lastOrNull()?.faceLevel,
        )
        val issues = mutableListOf<CallDiagnosisIssue>()
        val rttSteps = CallQuality.RTT_STEPS
        val lossSteps = CallQuality.LOSS_STEPS
        val jitterSteps = CallQuality.JITTER_STEPS
        val rtt = metrics.rttMs
        if (pick { it.rttMs }.size >= need && rtt != null && rtt >= rttSteps[1]) {
            issues.add(
                make(
                    "latency", if (rtt >= rttSteps[2]) CallDiagnosisSeverity.BAD else CallDiagnosisSeverity.WARN,
                    "Round trip ${round(rtt)} ms; under ${text(rttSteps[0])} ms feels natural.",
                ),
            )
        }
        val loss = metrics.lossPct
        if (pick { it.lossPct }.size >= need && loss != null && loss >= lossSteps[1]) {
            issues.add(
                make(
                    "loss", if (loss >= lossSteps[2]) CallDiagnosisSeverity.BAD else CallDiagnosisSeverity.WARN,
                    "${fixed(loss, 1)}% of packets lost; under ${text(lossSteps[0])}% is fine.",
                ),
            )
        }
        val jitter = metrics.jitterMs
        if (pick { it.jitterMs }.size >= need && jitter != null && jitter >= jitterSteps[1]) {
            issues.add(
                make(
                    "jitter", if (jitter >= jitterSteps[2]) CallDiagnosisSeverity.BAD else CallDiagnosisSeverity.WARN,
                    "${round(jitter)} ms jitter; under ${text(jitterSteps[0])} ms is fine.",
                ),
            )
        }
        val newest = last.lastOrNull()
        if (video && sending && last.size >= need && newest != null) {
            fun limited(reason: String): Boolean = last.count { it.limit == reason } * 2 > last.size
            val described = describeOut(newest, metrics.outFps, metrics.sendKbps)
            val out = described.ifEmpty { "reduced" }
            if (limited("bandwidth")) issues.add(make("bandwidth", CallDiagnosisSeverity.WARN, "Video out is $out, limited by bandwidth."))
            if (limited("cpu")) issues.add(make("cpu", CallDiagnosisSeverity.WARN, "Video out is $out, limited by CPU."))
            val fps = metrics.outFps
            if (pick { it.outFps }.size >= need && fps != null && fps < VIDEO_MIN_FPS) {
                issues.add(
                    make(
                        "sendFps", if (fps < VIDEO_MIN_FPS / 2) CallDiagnosisSeverity.BAD else CallDiagnosisSeverity.WARN,
                        "Your video leaves at ${round(fps)} fps; ${text(VIDEO_MIN_FPS)} or more looks smooth.",
                    ),
                )
            }
        }
        val recv = metrics.recvKbps
        if (video && pick { it.recvKbps }.size >= need && recv != null && recv >= VIDEO_MIN_KBPS) {
            val slow = pick { it.inFps }.size >= need && (metrics.inFps ?: Double.POSITIVE_INFINITY) < VIDEO_MIN_FPS
            val small = inHeights.size >= need && (metrics.inHeight ?: Double.POSITIVE_INFINITY) < VIDEO_MIN_HEIGHT
            if (slow || small) {
                val parts = mutableListOf<String>()
                val height = metrics.inHeight
                if (small && height != null) parts.add("${round(height)}p")
                val inFps = metrics.inFps
                if (slow && inFps != null) parts.add("${round(inFps)} fps")
                issues.add(make("receive", CallDiagnosisSeverity.WARN, "Their video arrives at ${parts.joinToString(", ")}."))
            }
        }
        val face = metrics.faceMs
        if (pick { it.faceMs }.size >= need && face != null && face > FACE_SLOW_MS) {
            val fps = metrics.faceFps?.let { " and draws ${round(it)} frames a second" }.orEmpty()
            issues.add(
                make(
                    "face", if (face > FACE_BAD_MS) CallDiagnosisSeverity.BAD else CallDiagnosisSeverity.WARN,
                    "The face filter takes ${round(face)} ms a frame$fps; under ${text(FACE_SLOW_MS)} ms keeps up with the camera.",
                ),
            )
        }
        val ordered = issues.filter { it.severity == CallDiagnosisSeverity.BAD } + issues.filter { it.severity != CallDiagnosisSeverity.BAD }
        val severity = when {
            ordered.any { it.severity == CallDiagnosisSeverity.BAD } -> CallDiagnosisSeverity.BAD
            ordered.isNotEmpty() -> CallDiagnosisSeverity.WARN
            last.size >= need -> CallDiagnosisSeverity.OK
            else -> CallDiagnosisSeverity.UNKNOWN
        }
        return CallDiagnosis(at = newest?.at, severity = severity, issues = ordered, metrics = metrics)
    }

    fun rankFixes(issue: CallDiagnosisIssue, memory: CallDiagnosticsMemory): List<CallDiagnosisFix> =
        issue.fixes.mapIndexed { index, fix ->
            val record = memory.record(issue.id, fix.id)
            Triple(record.worked * 2 - record.failed, index, fix.copy(worked = record.worked, failed = record.failed))
        }.sortedWith(compareByDescending<Triple<Int, Int, CallDiagnosisFix>> { it.first }.thenBy { it.second }).map { it.third }

    fun recordFixOutcome(memory: CallDiagnosticsMemory, issue: String, fix: String, worked: Boolean) {
        val record = memory.record(issue, fix)
        memory.fixes["$issue:$fix"] = if (worked) record.copy(worked = record.worked + 1) else record.copy(failed = record.failed + 1)
    }

    fun rememberReport(memory: CallDiagnosticsMemory, diagnosis: CallDiagnosis, now: Double = System.currentTimeMillis() / 1000.0) {
        val metrics = LinkedHashMap<String, Any?>()
        for ((key, value) in diagnosis.metrics.entries) {
            if (value == null) continue
            metrics[key] = number(fixed(value, 1).toDoubleOrNull() ?: value)
        }
        memory.reports.add(CallDiagnosticsReport(diagnosis.at ?: now, diagnosis.issues.map { it.id }, metrics))
        while (memory.reports.size > REPORTS) memory.reports.removeAt(0)
    }

    private val LESSON_PREFIX = Regex("^\\s*lesson\\s*:\\s*", RegexOption.IGNORE_CASE)
    private val SPACES = Regex("\\s+")

    fun rememberNote(memory: CallDiagnosticsMemory, text: String): Boolean {
        var note = LESSON_PREFIX.replaceFirst(text, "")
        note = SPACES.replace(note, " ").trim()
        if (note.length > NOTE_CHARS) note = note.take(NOTE_CHARS)
        if (note.isEmpty()) return false
        memory.notes.removeAll { it == note }
        memory.notes.add(note)
        while (memory.notes.size > NOTES) memory.notes.removeAt(0)
        return true
    }

    fun describe(metrics: CallDiagnosisMetrics): List<String> {
        val lines = mutableListOf<String>()
        metrics.rttMs?.let { rtt ->
            var line = "Network: ${fixed(rtt, 0)} ms round trip"
            metrics.lossPct?.let { line += ", ${fixed(it, 1)}% loss" }
            metrics.jitterMs?.let { line += ", ${fixed(it, 0)} ms jitter" }
            lines.add(line)
        }
        if (metrics.outFps != null || metrics.sendKbps != null) {
            val parts = listOf(
                metrics.outHeight?.let { fixed(it, 0) + "p" }.orEmpty(),
                metrics.outFps?.let { fixed(it, 0) + " fps" }.orEmpty(),
                formatKbps(metrics.sendKbps),
            ).filter { it.isNotEmpty() }
            lines.add("Video out: " + parts.joinToString(", "))
        }
        if (metrics.inFps != null || metrics.recvKbps != null) {
            val parts = listOf(
                metrics.inHeight?.let { fixed(it, 0) + "p" }.orEmpty(),
                metrics.inFps?.let { fixed(it, 0) + " fps" }.orEmpty(),
                formatKbps(metrics.recvKbps),
            ).filter { it.isNotEmpty() }
            lines.add("Video in: " + parts.joinToString(", "))
        }
        metrics.faceMs?.let { face ->
            var line = "Face filter: ${fixed(face, 0)} ms a frame"
            metrics.faceFps?.let { line += ", ${fixed(it, 0)} fps" }
            metrics.faceLevel?.let { line += ", quality step ${text(it)}" }
            lines.add(line)
        }
        return lines
    }

    fun earlierReports(diagnosis: CallDiagnosis, memory: CallDiagnosticsMemory): List<CallDiagnosticsReport> =
        memory.reports.filter { diagnosis.at == null || it.at != diagnosis.at }

    fun promptLabel(diagnosis: CallDiagnosis, memory: CallDiagnosticsMemory): String {
        val earlier = earlierReports(diagnosis, memory).size
        return "Prompt for an AI assistant · v${earlier + 1}, learned from $earlier earlier report${if (earlier == 1) "" else "s"}"
    }

    fun prompt(
        diagnosis: CallDiagnosis,
        memory: CallDiagnosticsMemory,
        environment: CallDiagnosticsEnvironment = CallDiagnosticsEnvironment(),
    ): String {
        val earlier = earlierReports(diagnosis, memory)
        val lines = mutableListOf<String>()
        lines.add("Prompt v${earlier.size + 1} · learned from ${earlier.size} earlier report${if (earlier.size == 1) "" else "s"} on this device.")
        lines.add("")
        lines.add(
            "You are helping me fix the quality of a ForkMesh call (WebRTC in the browser, with an optional on-device face filter). Measurements from the last few seconds of my call:",
        )
        for (line in describe(diagnosis.metrics)) lines.add("- $line")
        val device = listOf(
            environment.userAgent,
            if (environment.cores != 0) "${environment.cores} CPU cores" else "",
            if (environment.memory != 0.0) "${text(environment.memory)} GB memory" else "",
            environment.connection,
        ).filter { it.isNotEmpty() }
        if (device.isNotEmpty()) lines.add("- Device: " + device.joinToString("; "))
        lines.add("")
        if (diagnosis.issues.isEmpty()) {
            lines.add("No problem was detected automatically. If the call still feels wrong to me, help me find out why.")
        } else {
            lines.add("Problems detected:")
            diagnosis.issues.forEachIndexed { index, issue ->
                val seen = earlier.count { issue.id in it.issues }
                lines.add(
                    "${index + 1}. ${issue.title} (${if (issue.severity == CallDiagnosisSeverity.BAD) "severe" else "noticeable"}, ${issue.area}). ${issue.detail}",
                )
                if (seen > 0) lines.add("   Seen in $seen of ${earlier.size} earlier reports on this device.")
                val ranked = rankFixes(issue, memory)
                val worked = ranked.filter { it.worked > 0 }
                val failed = ranked.filter { it.failed > 0 && it.worked == 0 }
                if (worked.isNotEmpty()) lines.add("   Worked here before: " + worked.joinToString(" ") { "${it.text} (${it.worked}×)" })
                if (failed.isNotEmpty()) {
                    lines.add("   Tried here without clearing it: " + failed.joinToString(" ") { "${it.text} (${it.failed}×)" })
                }
                lines.add("   Suggested order: " + ranked.joinToString(" ") { it.text })
            }
        }
        if (memory.notes.isNotEmpty()) {
            lines.add("")
            lines.add("Lessons saved from earlier calls on this device (newest last):")
            for (note in memory.notes) lines.add("- $note")
        }
        lines.add("")
        lines.add("Please:")
        lines.add("1. Say in one or two sentences what most likely causes this, using the numbers above.")
        lines.add(
            "2. Give me the steps to fix it, most likely first. Start from what worked here before, and skip what was tried without clearing it unless something has changed.",
        )
        lines.add(
            "3. If it looks like a ForkMesh bug rather than my network or device, say which part (network, camera, encoder or face filter) and what to look for in chrome://webrtc-internals.",
        )
        lines.add(
            "4. End with one line that starts with \"LESSON:\" saying what to try first next time on this device. I will save it so the next version of this prompt starts from it.",
        )
        return lines.joinToString("\n")
    }

    fun fixLabel(fix: CallDiagnosisFix): String =
        fix.text + when {
            fix.worked > 0 -> " (worked here before)"
            fix.failed > 0 -> " (tried here before)"
            else -> ""
        }
}

class CallDiagnosticsTracker {
    private val waiting = mutableListOf<Pair<String, String>>()

    val pending: List<Pair<String, String>> get() = waiting.toList()

    fun opened(diagnosis: CallDiagnosis, memory: CallDiagnosticsMemory) {
        val at = diagnosis.at
        if (at != null && memory.reports.lastOrNull()?.at != at) CallDiagnostics.rememberReport(memory, diagnosis)
        for (issue in diagnosis.issues) {
            if (waiting.any { it.first == issue.id }) continue
            val first = CallDiagnostics.rankFixes(issue, memory).firstOrNull() ?: continue
            waiting.add(issue.id to first.id)
        }
    }

    fun refreshed(diagnosis: CallDiagnosis, memory: CallDiagnosticsMemory): Boolean {
        if (diagnosis.severity == CallDiagnosisSeverity.UNKNOWN || waiting.isEmpty()) return false
        val present = diagnosis.issues.map { it.id }.toSet()
        var learned = false
        val iterator = waiting.iterator()
        while (iterator.hasNext()) {
            val entry = iterator.next()
            if (entry.first in present) continue
            CallDiagnostics.recordFixOutcome(memory, entry.first, entry.second, worked = true)
            iterator.remove()
            learned = true
        }
        return learned
    }

    fun finished(memory: CallDiagnosticsMemory): Boolean {
        if (waiting.isEmpty()) return false
        for (entry in waiting) CallDiagnostics.recordFixOutcome(memory, entry.first, entry.second, worked = false)
        waiting.clear()
        return true
    }
}
