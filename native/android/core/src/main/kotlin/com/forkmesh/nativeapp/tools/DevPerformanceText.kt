package com.forkmesh.nativeapp.tools

import kotlin.math.max

object DevPerformanceText {
    const val TITLE = "Performance monitor"
    const val INDICATOR_LABEL = "Open performance monitor"
    const val EMPTY_TITLE = "No slow events"
    const val NOTHING_RECORDED = "No slow performance events recorded."

    fun formatLog(events: List<PerformanceEvent>): String {
        if (events.isEmpty()) return NOTHING_RECORDED
        return events.joinToString("\n\n") { formatEvent(it) }
    }

    fun formatEvent(event: PerformanceEvent): String {
        val lines = mutableListOf(
            "[${event.kind.wireName}] ${event.name}",
            "duration: ${event.durationMs}ms",
            "timestamp: ${dartIsoLocal(event.timestampMs)}",
        )
        for ((key, value) in event.details) lines.add("$key: $value")
        if (event.stackTrace.isNotEmpty()) {
            lines.add("stack:")
            lines.add(event.stackTrace)
        }
        return lines.joinToString("\n")
    }

    fun emptyMessage(monitor: PerformanceMonitorService): String =
        "Stalls over ${monitor.stallThresholdMs} ms and operations over ${monitor.thresholdMs} ms will appear here."

    fun durationLabel(event: PerformanceEvent): String = "${event.durationMs}ms"

    fun cardSubtitle(event: PerformanceEvent): String = "${event.kind.wireName} at ${clockStamp(event.timestampMs)}"

    fun cardDetails(event: PerformanceEvent): String = event.details.entries.joinToString("\n") { "${it.key}: ${it.value}" }

    fun loggedTo(path: String): String = "Logged to $path"

    fun showsIndicator(monitor: PerformanceMonitorService): Boolean = monitor.enabled && monitor.hasEvents

    fun clampIndicator(
        x: Float,
        y: Float,
        boundsWidth: Float,
        boundsHeight: Float,
        indicatorWidth: Float,
        indicatorHeight: Float,
        paddingTop: Float,
        paddingBottom: Float,
        margin: Float,
    ): Pair<Float, Float> {
        val minX = margin
        val maxX = max(minX, boundsWidth - indicatorWidth - margin)
        val minY = paddingTop + margin
        val maxY = max(minY, boundsHeight - indicatorHeight - paddingBottom - margin)
        return x.coerceIn(minX, maxX) to y.coerceIn(minY, maxY)
    }
}
