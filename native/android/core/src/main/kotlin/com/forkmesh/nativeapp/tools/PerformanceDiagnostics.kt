package com.forkmesh.nativeapp.tools

import java.util.Locale

data class PerformanceAlert(val reason: String) {
    val detected: Boolean get() = reason.isNotEmpty()

    val label: String get() = if (detected) "Slow, send report" else "Send diagnostics"

    data class Sample(val kind: PerformanceEventKind, val durationMs: Long, val atMs: Long) {
        constructor(event: PerformanceEvent) : this(event.kind, event.durationMs, event.timestampMs)
    }

    data class Diagnostic(val summary: String, val atMs: Long)

    companion object {
        const val HANG_MS = 250L
        const val HITCH_COUNT = 5
        const val WINDOW_MS = 10 * 60 * 1000L
        const val DIAGNOSTIC_WINDOW_MS = 24 * 60 * 60 * 1000L
        val NONE = PerformanceAlert("")

        fun evaluate(samples: List<Sample>, diagnostics: List<Diagnostic>, lastSentMs: Long?, nowMs: Long): PerformanceAlert {
            val sent = lastSentMs ?: Long.MIN_VALUE
            val since = maxOf(nowMs - WINDOW_MS, sent)
            val diagnosticSince = maxOf(nowMs - DIAGNOSTIC_WINDOW_MS, sent)
            diagnostics.filter { it.atMs > diagnosticSince && it.atMs <= nowMs }.maxByOrNull { it.atMs }?.let {
                return PerformanceAlert("Android reported: ${it.summary}")
            }
            val recent = samples.filter { it.atMs > since && it.atMs <= nowMs }
            val hangs = recent.filter {
                (it.kind == PerformanceEventKind.EventLoop || it.kind == PerformanceEventKind.Sync) && it.durationMs >= HANG_MS
            }
            hangs.maxByOrNull { it.durationMs }?.let { worst ->
                val seconds = String.format(Locale.US, "%.1f", worst.durationMs / 1000.0)
                val extra = if (hangs.size > 1) " (${hangs.size} freezes)" else ""
                return PerformanceAlert("The app froze for $seconds s$extra")
            }
            val hitches = recent.count { it.kind == PerformanceEventKind.Frame }
            if (hitches >= HITCH_COUNT) return PerformanceAlert("$hitches dropped-frame hitches in ${WINDOW_MS / 60_000} min")
            return NONE
        }
    }
}

object DiagnosticsBundle {
    const val REPOSITORY = "forkmesh/forkmesh"
    const val MAX_PROMPT_CHARS = 8000
    const val LOG_LINES = 15
    const val SLOW_EVENTS = 8
    val LOG_SECTIONS = listOf("## App log")

    private val rules: List<Pair<Regex, String>> = listOf(
        Regex("(?i)\\bbearer\\s+[A-Za-z0-9._~+/=-]+") to "Bearer [redacted]",
        Regex("(?i)\\b(authorization|bearer|token|access_token|refresh_token|secret|password|passwd|api[_-]?key|cookie|session(?:_?id)?|signature|sig)\\b([\"']?\\s*[:=]\\s*[\"']?)[^\\s\"',;&]+") to "$1$2[redacted]",
        Regex("\\beyJ[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]*") to "[token]",
        Regex("(?i)\"(text|body|message|content|prompt|title|note|subject)\"\\s*:\\s*\"(?:[^\"\\\\]|\\\\.)*\"") to "\"$1\":\"[redacted]\"",
        Regex("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}") to "[email]",
        Regex("(https?://[^\\s?#\"]+)\\?[^\\s\"]*") to "$1?[query]",
        Regex("\\+\\d[\\d ()-]{8,}\\d") to "[number]",
        Regex("\\b[A-Za-z0-9_-]{32,}\\b") to "[id]",
        Regex("/(data/user/\\d+|data/data|storage/emulated/\\d+)/[^/\\s]+") to "/$1/~",
    )

    fun scrub(text: String): String = rules.fold(text) { acc, (pattern, replacement) -> pattern.replace(acc, replacement) }

    fun dropSections(text: String, titles: List<String>): String {
        var skipping = false
        return text.split("\n").filter { line ->
            if (line.startsWith("## ")) skipping = titles.any { line.startsWith(it) }
            !skipping
        }.joinToString("\n")
    }

    fun lead(reason: String): String {
        val why = if (reason.isEmpty()) "The user asked for a performance check" else "The app detected a performance issue: $reason"
        return "$why. Diagnose what makes the ForkMesh native Android app (native/android in $REPOSITORY) slow on this phone, " +
            "using the diagnostics bundle below. Fix the cause on a branch with tests, and explain what you found. " +
            "The bundle was scrubbed of tokens, emails and message contents on the phone."
    }

    fun report(
        context: List<Pair<String, String>>,
        telemetry: AppTelemetry?,
        events: List<PerformanceEvent>,
        diagnostics: List<PerformanceAlert.Diagnostic>,
        nowMs: Long,
    ): String {
        val out = StringBuilder("# ForkMesh Android performance report\n")
        context.forEach { (key, value) -> out.append("$key: $value\n") }
        out.append("\n## Metrics (5m | 1h: avg / max)\n")
        if (telemetry == null) {
            out.append("telemetry is off\n")
        } else {
            for (metric in listOf(TelemetryMetric.Fps, TelemetryMetric.Frame, TelemetryMetric.Loop, TelemetryMetric.Mem, TelemetryMetric.Net)) {
                val short = telemetry.summary(metric, TelemetryRange.M5)
                val long = telemetry.summary(metric, TelemetryRange.H1)
                out.append("${metric.label}: ${fmt(short.avg)} / ${fmt(short.max)} | ${fmt(long.avg)} / ${fmt(long.max)} ${metric.unit}".trimEnd()).append('\n')
            }
        }
        val recent = events.filter { nowMs - it.timestampMs <= PerformanceAlert.WINDOW_MS }
        out.append("\n## Slowest events, last ${PerformanceAlert.WINDOW_MS / 60_000} min\n")
        if (recent.isEmpty()) out.append("none\n")
        recent.sortedByDescending { it.durationMicros }.take(SLOW_EVENTS).forEach { event ->
            out.append("${event.kind.wireName} ${event.name} ${event.durationMs}ms ${(nowMs - event.timestampMs) / 1000}s ago\n")
            val stack = event.stackTrace.lineSequence().map { it.trim() }.filter { it.isNotEmpty() }.take(3).toList()
            if (stack.isNotEmpty()) out.append("stack: ${stack.joinToString(" < ")}\n")
        }
        out.append("\n## Exit reasons from Android\n")
        if (diagnostics.isEmpty()) out.append("none\n")
        diagnostics.sortedByDescending { it.atMs }.take(3).forEach { out.append("${(nowMs - it.atMs) / 60_000} min ago: ${it.summary}\n") }
        out.append("\n## App log, last $LOG_LINES lines\n")
        val lines = telemetry?.log?.entries()?.takeLast(LOG_LINES).orEmpty()
        if (lines.isEmpty()) out.append("empty\n")
        lines.forEach { out.append(it.toString().take(160)).append('\n') }
        return out.toString()
    }

    fun prompt(reason: String, report: String, includeLogs: Boolean, limit: Int = MAX_PROMPT_CHARS): String {
        val body = scrub(if (includeLogs) report else dropSections(report, LOG_SECTIONS))
        val head = lead(reason) + "\n\n# Diagnostics bundle\n"
        val room = maxOf(0, limit - head.length)
        val clipped = if (body.length <= room) body else body.take(maxOf(0, room - 24)) + "\n[cut to fit the prompt]"
        return head + clipped
    }

    private fun fmt(value: Double): String =
        if (value.isNaN() || value.isInfinite()) "-" else String.format(Locale.US, "%.0f", value)
}
