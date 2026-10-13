package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.hosts.RemoteFormat
import kotlin.math.abs
import kotlin.math.roundToLong

object StatsFormat {
    fun compactNumber(value: Double): String {
        val magnitude = abs(value)
        if (magnitude < 1000) {
            if (abs(value - Math.rint(value)) < 0.05) return grouped(Math.rint(value).toLong())
            return RemoteFormat.fixed(value, 1)
        }
        if (magnitude < 10_000) return grouped(value.roundToLong())
        if (magnitude < 1_000_000) return RemoteFormat.fixed(value / 1000, if (magnitude < 100_000) 1 else 0) + "K"
        if (magnitude < 1_000_000_000) return RemoteFormat.fixed(value / 1_000_000, if (magnitude < 10_000_000) 2 else 1) + "M"
        return RemoteFormat.fixed(value / 1_000_000_000, 1) + "B"
    }

    fun compactBytes(bytes: Long): String {
        val units = listOf("B", "KB", "MB", "GB", "TB")
        var value = maxOf(0L, bytes).toDouble()
        var unit = 0
        while (value >= 1024 && unit < 4) {
            value /= 1024
            unit++
        }
        if (unit == 0) return "${value.toLong()} B"
        return RemoteFormat.fixed(value, if (value < 10) 1 else 0) + " " + units[unit]
    }

    fun compactUsd(value: Double): String {
        val magnitude = abs(value)
        val sign = if (value < 0) "-" else ""
        if (magnitude < 100) return sign + "$" + RemoteFormat.fixed(magnitude, 2)
        return "$sign$" + compactNumber(magnitude)
    }

    fun compactDuration(ms: Long): String {
        val seconds = maxOf(0L, ms / 1000)
        if (seconds < 60) return "${seconds}s"
        val minutes = seconds / 60
        if (minutes < 60) return "${minutes}m"
        val hours = minutes / 60
        if (hours < 48) return "${hours}h ${minutes % 60}m"
        return "${hours / 24}d ${hours % 24}h"
    }

    fun trendPercent(values: List<Double>, categorical: Boolean = false): Double? {
        val n = values.size
        if (categorical || n < 4) return null
        val half = n / 2
        var older = 0.0
        var newer = 0.0
        for (i in 0 until half) {
            older += values[i]
            newer += values[n - half + i]
        }
        older /= half
        newer /= half
        if (abs(older) < 1e-9) return null
        return 100 * (newer - older) / abs(older)
    }

    fun percent(value: Double?, digits: Int = 1): String =
        if (value == null) "–" else RemoteFormat.fixed(value, digits) + "%"

    private fun grouped(value: Long): String {
        val negative = value < 0
        val digits = (if (negative) -value else value).toString()
        val out = StringBuilder()
        for ((index, char) in digits.withIndex()) {
            if (index > 0 && (digits.length - index) % 3 == 0) out.append(',')
            out.append(char)
        }
        return if (negative) "-$out" else out.toString()
    }
}

enum class StatsTone { None, Good, Warning, Critical }

enum class StatsTrendPolarity { Neutral, UpIsGood, DownIsGood }

data class StatsTileCatalogEntry(val id: String, val title: String, val group: String, val hint: String, val icon: String)

data class StatsTileSeries(val labels: List<String> = emptyList(), val values: List<Double> = emptyList(), val categorical: Boolean = false) {
    val isEmpty: Boolean get() = values.isEmpty()
}

data class StatsTileData(
    val value: String = "",
    val note: String = "",
    val series: StatsTileSeries = StatsTileSeries(),
    val available: Boolean = true,
    val status: StatsTone = StatsTone.None,
    val trend: StatsTrendPolarity = StatsTrendPolarity.Neutral,
) {
    val trendPercent: Double? get() = StatsFormat.trendPercent(series.values, series.categorical)

    val trendTone: StatsTone
        get() {
            val percent = trendPercent ?: return StatsTone.None
            if (abs(percent) < 1) return StatsTone.None
            return when (trend) {
                StatsTrendPolarity.Neutral -> StatsTone.None
                StatsTrendPolarity.UpIsGood -> if (percent > 0) StatsTone.Good else StatsTone.Warning
                StatsTrendPolarity.DownIsGood -> if (percent < 0) StatsTone.Good else StatsTone.Warning
            }
        }

    companion object {
        val unavailable = StatsTileData(available = false)
    }
}

data class StatsTilePlacement(val id: String, val width: Int = 1, val height: Int = 1) {
    companion object {
        const val MAX_SPAN = 2

        fun clamp(span: Int): Int = span.coerceIn(1, MAX_SPAN)

        fun of(id: String, width: Int = 1, height: Int = 1) = StatsTilePlacement(id, clamp(width), clamp(height))
    }
}

data class StatsLayoutMerge(val layout: List<StatsTilePlacement>, val offered: List<String>, val changed: Boolean)

object StatsLayout {
    fun toJson(layout: List<StatsTilePlacement>): List<Map<String, Any?>> =
        layout.map { mapOf("id" to it.id, "w" to it.width.toLong(), "h" to it.height.toLong()) }

    fun fromJson(value: Any?): List<StatsTilePlacement> = (value as? List<*>).orEmpty().mapNotNull { item ->
        val json = item.asJsonMap()
        val id = json.str("id")
        if (id.isEmpty()) null else StatsTilePlacement.of(id, json.int("w", 1), json.int("h", 1))
    }

    fun merge(
        saved: List<StatsTilePlacement>,
        hasSaved: Boolean,
        defaults: List<StatsTilePlacement>,
        catalog: List<StatsTileCatalogEntry>,
        offered: List<String>,
    ): StatsLayoutMerge {
        val catalogIds = catalog.map { it.id }.toSet()
        val alreadyOffered = offered.toMutableSet()
        val placed = HashSet<String>()
        val layout = ArrayList<StatsTilePlacement>()
        var changed = false
        for (placement in if (hasSaved) saved else defaults) {
            if (placement.id.isEmpty() || placement.id in placed) continue
            placed.add(placement.id)
            layout.add(StatsTilePlacement.of(placement.id, placement.width, placement.height))
        }
        if (hasSaved) {
            for (placement in defaults) {
                if (placement.id.isEmpty() || placement.id in placed || placement.id in alreadyOffered || placement.id !in catalogIds) continue
                placed.add(placement.id)
                layout.add(placement)
                changed = true
            }
        } else {
            changed = true
        }
        val nextOffered = offered.toMutableList()
        for (entry in catalog) {
            if (entry.id in alreadyOffered) continue
            nextOffered.add(entry.id)
            alreadyOffered.add(entry.id)
            changed = true
        }
        return StatsLayoutMerge(layout, nextOffered, changed)
    }

    fun visible(layout: List<StatsTilePlacement>, catalog: List<StatsTileCatalogEntry>): List<StatsTilePlacement> {
        val ids = catalog.map { it.id }.toSet()
        return layout.filter { it.id in ids }
    }

    fun move(layout: List<StatsTilePlacement>, id: String, to: Int): List<StatsTilePlacement> {
        val from = layout.indexOfFirst { it.id == id }
        if (from < 0) return layout
        val next = layout.toMutableList()
        val item = next.removeAt(from)
        next.add(to.coerceIn(0, next.size), item)
        return next
    }
}

object StatsCatalog {
    val entries: List<StatsTileCatalogEntry> = listOf(
        StatsTileCatalogEntry("repos", "Repositories", "Network", "Public repositories in the network catalog", "inventory_2_outlined"),
        StatsTileCatalogEntry("nodes", "Nodes online", "Network", "Nodes connected to the relay right now", "hub_outlined"),
        StatsTileCatalogEntry("hosts", "Hosts online", "Network", "Nodes serving repositories right now", "dns_outlined"),
        StatsTileCatalogEntry("nodeHours", "Node activity", "Network", "Node-minutes online per hour, last 24 hours", "bar_chart"),
        StatsTileCatalogEntry("uptime", "Uptime (24 h)", "Status", "Share of the website's checks that passed in the last 24 hours", "verified_outlined"),
        StatsTileCatalogEntry("errors", "Errors (24 h)", "Status", "Server errors logged in the last 24 hours", "warning_amber"),
        StatsTileCatalogEntry("incidents", "Systems down", "Status", "Monitored systems failing their latest check", "bolt"),
        StatsTileCatalogEntry("desktopLoad", "Desktop load", "Desktop", "CPU of your paired desktop", "memory"),
        StatsTileCatalogEntry("desktopMemory", "Desktop memory", "Desktop", "Memory in use on your paired desktop", "sd_storage_outlined"),
        StatsTileCatalogEntry("agents", "Agents running", "Desktop", "Agents running and queued on your paired desktop", "auto_awesome"),
        StatsTileCatalogEntry("stalls", "Desktop stalls", "Desktop", "UI stalls your desktop recorded", "hourglass_empty"),
    )

    val defaults: List<StatsTilePlacement> = listOf(
        StatsTilePlacement("nodes"), StatsTilePlacement("repos"),
        StatsTilePlacement("uptime"), StatsTilePlacement("errors"),
        StatsTilePlacement("nodeHours", width = 2), StatsTilePlacement("agents"),
        StatsTilePlacement("desktopLoad"), StatsTilePlacement("hosts"),
        StatsTilePlacement("incidents"),
    )

    fun entry(id: String): StatsTileCatalogEntry? = entries.firstOrNull { it.id == id }
}

class StatsLayoutStore(private val store: PrefStore, private val scope: String = "") {
    private fun key(base: String) = if (scope.isEmpty()) base else "$scope/$base"

    fun load(): StatsLayoutMerge {
        val raw = store.getString(key(LAYOUT_KEY))
        val saved = raw?.let { runCatching { Json.parse(it) }.getOrNull() }
        val offered = store.getString(key(OFFERED_KEY))
            ?.let { runCatching { Json.parse(it) }.getOrNull() as? List<*> }
            ?.map { it.toString() }
            .orEmpty()
        val merge = StatsLayout.merge(StatsLayout.fromJson(saved), saved != null, StatsCatalog.defaults, StatsCatalog.entries, offered)
        if (merge.changed) save(merge.layout, merge.offered)
        return merge
    }

    fun save(layout: List<StatsTilePlacement>, offered: List<String>) {
        store.putString(key(LAYOUT_KEY), Json.stringify(StatsLayout.toJson(layout)))
        store.putString(key(OFFERED_KEY), Json.stringify(offered))
    }

    fun reset() {
        store.remove(key(LAYOUT_KEY))
    }

    companion object {
        const val LAYOUT_KEY = "insights/stats/layout.v1"
        const val OFFERED_KEY = "insights/stats/offered.v1"
    }
}
