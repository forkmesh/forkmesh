package com.forkmesh.nativeapp.agentwatch

import kotlin.math.floor
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

const val kWatchLabelGap: Double = 19.0
const val kWatchLabelPixels: Double = 11.0
const val kWatchLedPitch: Double = 2.5
const val kWatchLedPad: Double = 3.0
const val kWatchLedHold: Double = 0.6
const val kWatchLedSpeed: Double = 150.0
const val kWatchLedGapDots: Int = 14
const val kWatchLedMaxWidth: Double = 220.0
const val kWatchLedRows: Int = 10
const val kWatchLedHeight: Double = kWatchLedRows * kWatchLedPitch + 2 * kWatchLedPad
private const val LED_ROOM: Double = kWatchLedHeight + 2.0
private const val ROWS_TOP = 0.06
private const val ROWS_BOTTOM = 0.80
const val kWatchBandHeader: Double = 0.035
private const val ROW_PIXELS = 150.0

data class WatchPoint(val x: Double, val y: Double)

data class WatchRect(val x: Double, val y: Double, val width: Double, val height: Double) {
    val maxX: Double get() = x + width
    val maxY: Double get() = y + height
    val midX: Double get() = x + width / 2
}

data class WatchRow(val band: WatchBand, val y: Double, val height: Double, val count: Int)

class WatchFieldLayout(val width: Double, val height: Double, agents: List<WatchedAgent>) {
    private class Seat(val x: Double, val y: Double, val slot: Double)

    val bounds: WatchRect
    val unit: Double
    val rows = ArrayList<WatchRow>()
    private val seats = HashMap<WatchedAgent, Seat>()
    private val seated = ArrayList<WatchedAgent>()

    init {
        val fieldWidth = max(60.0, width - 52)
        val fieldHeight = max(60.0, height - 28)
        bounds = WatchRect(26.0, 14.0, fieldWidth, fieldHeight)
        unit = min(fieldWidth, fieldHeight)
        val rowSize = rowSizeFor(width)
        val banded = List(WatchBand.entries.size) { ArrayList<WatchedAgent>() }
        for (agent in agents) banded[bandFor(agent.state).ordinal] += agent
        var rowCount = 0
        var bands = 0
        for (band in banded) {
            band.sortWith { a, b ->
                val ax = min(max(a.voice.pan, 0.0), 1.0)
                val bx = min(max(b.voice.pan, 0.0), 1.0)
                if (ax != bx) ax.compareTo(bx) else a.id.compareTo(b.id)
            }
            rowCount += (band.size + rowSize - 1) / rowSize
            if (band.isNotEmpty()) bands += 1
        }
        if (rowCount > 0) {
            val step = min(0.26, (ROWS_BOTTOM - ROWS_TOP - bands * kWatchBandHeader) / rowCount)
            var y = ROWS_TOP + step / 2
            banded.forEachIndexed { b, band ->
                if (band.isNotEmpty()) y += kWatchBandHeader
                var first = 0
                while (first < band.size) {
                    val count = min(rowSize, band.size - first)
                    rows += WatchRow(WatchBand.entries[b], y, step, if (first == 0) band.size else 0)
                    val slot = 0.88 / count
                    for (i in 0 until count) {
                        val agent = band[first + i]
                        seats[agent] = Seat(0.06 + slot * (i + 0.5), y, slot)
                        seated += agent
                    }
                    y += step
                    first += rowSize
                }
            }
        }
    }

    fun pointFor(agent: WatchedAgent): WatchPoint {
        val seat = seats[agent] ?: Seat(0.5, 0.5, 0.2)
        return WatchPoint(bounds.x + seat.x * bounds.width, bounds.y + seat.y * bounds.height - LED_ROOM / 2)
    }

    fun radiusFor(agent: WatchedAgent, now: Double): Double {
        val scale = unit / 520.0
        val base = 9.0 + 17.0 * agent.size + 9.0 * agent.energyAt(now)
        var most = 64.0
        rows.firstOrNull()?.let { first ->
            most = max(8.0, first.height * bounds.height / 2 - kWatchLabelGap - kWatchLabelPixels - 3.0 - LED_ROOM / 2)
        }
        return min(max(base * scale, 5.0), most)
    }

    val desktopNode: WatchPoint get() = WatchPoint(bounds.midX, bounds.maxY - 34.0 * max(0.8, unit / 520.0))

    fun agentAt(point: WatchPoint, now: Double, shown: ((WatchedAgent) -> Boolean)? = null): WatchedAgent? {
        var best: WatchedAgent? = null
        var bestDistance = Double.POSITIVE_INFINITY
        for (agent in seated) {
            if (shown != null && !shown(agent)) continue
            val at = pointFor(agent)
            val distance = hypot(at.x - point.x, at.y - point.y)
            val reach = radiusFor(agent, now) + kWatchLabelGap + 4
            if (distance <= reach && distance < bestDistance) {
                best = agent
                bestDistance = distance
            }
        }
        return best
    }

    fun ledRect(agent: WatchedAgent, centre: WatchPoint, radius: Double): WatchRect {
        val slot = seats[agent]?.slot ?: 0.2
        val room = min(max(slot * bounds.width - 12, 64.0), kWatchLedMaxWidth)
        val dots = floor((room - 2 * kWatchLedPad) / kWatchLedPitch)
        val w = dots * kWatchLedPitch + 2 * kWatchLedPad
        val x = roundHalfAway(centre.x - w / 2)
        val below = radius + kWatchLabelGap + kWatchLabelPixels + 2
        val y = roundHalfAway(centre.y + below)
        return WatchRect(x, y, w, kWatchLedHeight)
    }

    companion object {
        fun rowSizeFor(width: Double): Int = min(max(floor((width - 52) / 110).toInt(), 1), 8)

        fun heightFor(width: Double, agents: List<WatchedAgent>): Double {
            val rowSize = rowSizeFor(width)
            val counts = IntArray(WatchBand.entries.size)
            for (agent in agents) counts[bandFor(agent.state).ordinal] += 1
            var rows = 0
            var bands = 0
            for (count in counts) {
                rows += (count + rowSize - 1) / rowSize
                if (count > 0) bands += 1
            }
            val room = ROWS_BOTTOM - ROWS_TOP - bands * kWatchBandHeader
            return rows * ROW_PIXELS / room + 28
        }
    }
}

internal fun roundHalfAway(x: Double): Double = if (x >= 0) floor(x + 0.5) else -floor(-x + 0.5)

fun watchFaceName(provider: String, model: String): String {
    val m = model.trim().lowercase()
    val p = provider.trim().lowercase()
    for (face in listOf("opus", "sonnet", "haiku", "fable", "sol", "luna", "terra")) {
        if (m.contains(face)) return face
    }
    if (p.startsWith("claude") || m.startsWith("claude")) return "feather"
    if (m.contains("mini") || m.contains("nano")) return "crescent-moon"
    if (p == "codex" || m.contains("codex") || p == "openai") return "sun"
    return "globe-leaf"
}

fun watchTitleWithoutNumber(id: Int, text: String): String {
    var title = collapseWhitespace(text)
    val number = "#$id"
    if (title.startsWith(number)) title = title.substring(number.length).trim()
    while (title.startsWith("-") || title.startsWith(":")) title = title.substring(1).trim()
    return title
}

fun watchCardLines(controller: AgentWatchController, agent: WatchedAgent): List<String> {
    val project = agent.target.projectName
    val task = watchTitleWithoutNumber(agent.id, agent.target.title)
    val name = listOf(project, task).filter { it.isNotEmpty() }.joinToString(" · ")
    val lines = ArrayList<String>()
    lines += if (agent.id > 0) (if (name.isEmpty()) "#${agent.id}" else "#${agent.id} $name") else name
    val states = mapOf(
        WatchState.starting to "running",
        WatchState.running to "running",
        WatchState.waiting to "waiting",
        WatchState.done to "done",
        WatchState.failed to "failed",
    )
    val heard = agent.listening && controller.sound
    lines += "${states[agent.state] ?: ""} · ${if (heard) "playing as" else "would sound as"} ${agent.voice.description}"
    if (agent.target.subtitle.isNotEmpty()) lines += "on: ${agent.target.subtitle}"
    if (agent.turns > 0 || agent.tokens > 0 || agent.events > 0) {
        lines += "${agent.turns} turns · ${agent.tokens / 1000}k tokens · ${agent.events} events"
    }
    fun wrapped(label: String, text: String) {
        val words = text.split(Regex("\\s+")).filter { it.isNotEmpty() }
        val width = 64
        val maxLines = 4
        var line = label
        var used = 0
        for (word in words) {
            if (line.length + word.length + 1 > width && line.length > label.length) {
                used += 1
                if (used == maxLines) {
                    lines += "$line…"
                    return
                }
                lines += line
                line = "    "
            }
            line += (if (line.endsWith(" ")) "" else " ") + word
        }
        if (line.trim().isNotEmpty()) lines += line
    }
    if (agent.prompt.isNotEmpty()) wrapped("prompt:", agent.prompt)
    if (agent.lastPrompt.isNotEmpty() && agent.lastPrompt.trim() != agent.prompt.trim()) wrapped("latest:", agent.lastPrompt)
    if (agent.said.isNotEmpty()) wrapped("said:", agent.said)
    val doing = agent.last?.text ?: ""
    if (doing.isNotEmpty()) lines += "now: $doing"
    if (agent.error.isNotEmpty()) lines += "error: ${agent.error}"
    lines += "size: tokens burned · glow: how busy it is now"
    var rings = "icons round it: its latest marks, as the list of sounds draws them, newest at the top"
    if (agent.state == WatchState.running || agent.state == WatchState.starting) {
        rings += " · outer ring: running"
    } else if (agent.state == WatchState.waiting) {
        rings += " · dotted ring: waiting on you"
    }
    lines += rings
    lines += if (heard) "double tap to stop hearing it" else "double tap to hear it"
    return lines
}
