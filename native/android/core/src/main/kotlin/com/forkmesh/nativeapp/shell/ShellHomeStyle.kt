package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import java.text.Normalizer

interface ShellRaw {
    val raw: String
}

internal object ShellDecode {
    fun <E> enumOr(values: Map<String, Any?>, key: String, entries: List<E>, fallback: E): E where E : Enum<E>, E : ShellRaw {
        val text = values[key] as? String ?: return fallback
        return entries.firstOrNull { it.raw == text } ?: fallback
    }

    fun boolOr(values: Map<String, Any?>, key: String, fallback: Boolean): Boolean = values[key] as? Boolean ?: fallback

    fun intOr(values: Map<String, Any?>, key: String, fallback: Int): Int = when (val value = values[key]) {
        is Long -> if (value in Int.MIN_VALUE.toLong()..Int.MAX_VALUE.toLong()) value.toInt() else fallback
        is Double -> if (value == Math.rint(value) && value >= Int.MIN_VALUE && value <= Int.MAX_VALUE) value.toInt() else fallback
        else -> fallback
    }

    fun doubleOr(values: Map<String, Any?>, key: String, fallback: Double): Double =
        (values[key] as? Number)?.toDouble() ?: fallback

    fun stringsOr(values: Map<String, Any?>, key: String, fallback: List<String>): List<String> {
        val list = values[key] as? List<*> ?: return fallback
        if (list.any { it !is String }) return fallback
        return list.map { it as String }
    }

    @Suppress("UNCHECKED_CAST")
    fun parseObject(text: String?): Map<String, Any?>? {
        if (text == null) return null
        val value = try {
            Json.parse(text)
        } catch (_: Json.JsonException) {
            return null
        }
        return value as? Map<String, Any?>
    }

    fun write(store: PrefStore, key: String, value: Map<String, Any?>?) {
        if (value == null) store.remove(key) else store.putString(key, Json.stringify(value))
    }
}

fun shellNaturalCompare(left: String, right: String): Int {
    val x = fold(left)
    val y = fold(right)
    var i = 0
    var j = 0
    while (i < x.length && j < y.length) {
        val a = x[i]
        val b = y[j]
        if (a in '0'..'9' && b in '0'..'9') {
            var endX = i
            while (endX < x.length && x[endX] in '0'..'9') endX++
            var endY = j
            while (endY < y.length && y[endY] in '0'..'9') endY++
            val numberX = x.substring(i, endX).trimStart('0')
            val numberY = y.substring(j, endY).trimStart('0')
            if (numberX.length != numberY.length) return numberX.length.compareTo(numberY.length)
            val order = numberX.compareTo(numberY)
            if (order != 0) return order
            i = endX
            j = endY
        } else {
            if (a != b) return a.compareTo(b)
            i++
            j++
        }
    }
    return (x.length - i).compareTo(y.length - j)
}

private fun fold(text: String): String =
    Normalizer.normalize(text, Normalizer.Form.NFD).filter { Character.getType(it) != Character.NON_SPACING_MARK.toInt() }.lowercase()

enum class ShellHomeSort(override val raw: String, val title: String) : ShellRaw {
    custom("custom", "Custom order"),
    byName("name", "Name A–Z"),
    nameDescending("nameDescending", "Name Z–A"),
    mostUsed("mostUsed", "Most used"),
    leastUsed("leastUsed", "Least used"),
    recent("recent", "Recently used"),
    attention("attention", "Needs attention"),
    count("count", "Biggest count");

    companion object {
        fun fromRaw(raw: String?): ShellHomeSort? = entries.firstOrNull { it.raw == raw }
    }
}

enum class ShellHomeGroupOrder(override val raw: String, val title: String) : ShellRaw {
    usual("usual", "Usual order"),
    byName("name", "Name A–Z"),
    mostUsed("mostUsed", "Most used first"),
    size("size", "Most icons first");

    companion object {
        fun fromRaw(raw: String?): ShellHomeGroupOrder? = entries.firstOrNull { it.raw == raw }
    }
}

enum class ShellHomeIconStyle(override val raw: String, val title: String) : ShellRaw {
    outline("outline", "Outline"),
    filled("filled", "Filled"),
    accent("accent", "Accent"),
    tiles("tiles", "Tiles");

    companion object {
        fun fromRaw(raw: String?): ShellHomeIconStyle? = entries.firstOrNull { it.raw == raw }
    }
}

data class ShellHomeEntry(
    val id: String,
    val title: String,
    val badge: Int = 0,
    val count: Int? = null,
    val active: Boolean = false,
)

data class ShellHomeSection(val id: String, val title: String, val entries: List<ShellHomeEntry>)

data class ShellHomeArranged(val id: String, val title: String, val ids: List<String>)

data class ShellHomeStyle(
    var sort: ShellHomeSort = ShellHomeSort.custom,
    var grouped: Boolean = true,
    var showTitles: Boolean = true,
    var groupOrder: ShellHomeGroupOrder = ShellHomeGroupOrder.usual,
    var hidden: List<String> = emptyList(),
    var icons: ShellHomeIconStyle = ShellHomeIconStyle.outline,
    var across: Int = 5,
    var showCounts: Boolean = true,
    var highlightFrequent: Boolean = true,
    var hideBarIcons: Boolean = true,
) {
    val isDefault: Boolean get() = this == ShellHomeStyle()

    val reorders: Boolean get() = grouped

    val headed: Boolean get() = grouped && showTitles

    fun shows(group: String): Boolean = !hidden.contains(group)

    fun setShown(group: String, shown: Boolean) {
        hidden = hidden.filter { it != group }
        if (!shown) hidden = hidden + group
    }

    fun save(store: PrefStore) {
        ShellDecode.write(store, KEY, if (this == ShellHomeStyle()) null else toJson())
    }

    fun toJson(): Map<String, Any?> = linkedMapOf(
        "sort" to sort.raw,
        "grouped" to grouped,
        "showTitles" to showTitles,
        "groupOrder" to groupOrder.raw,
        "hidden" to hidden,
        "icons" to icons.raw,
        "across" to across,
        "showCounts" to showCounts,
        "highlightFrequent" to highlightFrequent,
        "hideBarIcons" to hideBarIcons,
    )

    fun arranged(
        sections: List<ShellHomeSection>,
        uses: Map<String, Int>,
        recent: List<String>,
        docked: Set<String> = emptySet(),
        searching: Boolean = false,
    ): List<ShellHomeArranged> {
        val shown = if (searching) sections else undocked(sections.filter { shows(it.id) }, docked)
        if (!grouped) {
            val entries = ordered(shown, groupOrder, uses).flatMap { it.entries }
            val ids = sorted(entries, sort, uses, recent).map { it.id }
            return if (ids.isEmpty()) emptyList() else listOf(ShellHomeArranged(ALL_ID, "", ids))
        }
        return ordered(shown, groupOrder, uses).mapNotNull { section ->
            val ids = sorted(section.entries, sort, uses, recent).map { it.id }
            if (ids.isEmpty()) null else ShellHomeArranged(section.id, section.title, ids)
        }
    }

    private fun undocked(sections: List<ShellHomeSection>, docked: Set<String>): List<ShellHomeSection> {
        if (!hideBarIcons || docked.isEmpty()) return sections
        return sections.map { ShellHomeSection(it.id, it.title, it.entries.filter { entry -> !docked.contains(entry.id) }) }
    }

    companion object {
        const val KEY = "shell/home-style"
        const val ALL_ID = "all"
        val ACROSS_CHOICES: List<Int> = listOf(4, 5, 6)

        fun clampedAcross(value: Int): Int = minOf(ACROSS_CHOICES.last(), maxOf(ACROSS_CHOICES.first(), value))

        fun fromJson(values: Map<String, Any?>): ShellHomeStyle {
            val base = ShellHomeStyle()
            return ShellHomeStyle(
                sort = ShellDecode.enumOr(values, "sort", ShellHomeSort.entries, base.sort),
                grouped = ShellDecode.boolOr(values, "grouped", base.grouped),
                showTitles = ShellDecode.boolOr(values, "showTitles", base.showTitles),
                groupOrder = ShellDecode.enumOr(values, "groupOrder", ShellHomeGroupOrder.entries, base.groupOrder),
                hidden = ShellDecode.stringsOr(values, "hidden", base.hidden),
                icons = ShellDecode.enumOr(values, "icons", ShellHomeIconStyle.entries, base.icons),
                across = clampedAcross(ShellDecode.intOr(values, "across", base.across)),
                showCounts = ShellDecode.boolOr(values, "showCounts", base.showCounts),
                highlightFrequent = ShellDecode.boolOr(values, "highlightFrequent", base.highlightFrequent),
                hideBarIcons = ShellDecode.boolOr(values, "hideBarIcons", base.hideBarIcons),
            )
        }

        fun load(store: PrefStore): ShellHomeStyle =
            ShellDecode.parseObject(store.getString(KEY))?.let(::fromJson) ?: ShellHomeStyle()

        fun ordered(sections: List<ShellHomeSection>, order: ShellHomeGroupOrder, uses: Map<String, Int>): List<ShellHomeSection> {
            fun total(section: ShellHomeSection): Int = section.entries.sumOf { uses[it.id] ?: 0 }
            return when (order) {
                ShellHomeGroupOrder.usual -> sections
                ShellHomeGroupOrder.byName -> sections.sortedWith { a, b -> shellNaturalCompare(a.title, b.title) }
                ShellHomeGroupOrder.mostUsed -> sections.sortedWith { a, b -> total(b).compareTo(total(a)) }
                ShellHomeGroupOrder.size -> sections.sortedWith { a, b -> b.entries.size.compareTo(a.entries.size) }
            }
        }

        fun sorted(entries: List<ShellHomeEntry>, sort: ShellHomeSort, uses: Map<String, Int>, recent: List<String>): List<ShellHomeEntry> {
            fun by(key: (ShellHomeEntry) -> Int, descending: Boolean): List<ShellHomeEntry> =
                entries.sortedWith { a, b -> if (descending) key(b).compareTo(key(a)) else key(a).compareTo(key(b)) }

            fun byName(ascending: Boolean): List<ShellHomeEntry> =
                entries.sortedWith { a, b ->
                    val order = shellNaturalCompare(a.title, b.title)
                    if (ascending) order else -order
                }
            return when (sort) {
                ShellHomeSort.custom -> entries
                ShellHomeSort.byName -> byName(true)
                ShellHomeSort.nameDescending -> byName(false)
                ShellHomeSort.mostUsed -> by({ uses[it.id] ?: 0 }, true)
                ShellHomeSort.leastUsed -> by({ uses[it.id] ?: 0 }, false)
                ShellHomeSort.recent -> {
                    val place = HashMap<String, Int>()
                    recent.forEachIndexed { index, id -> if (!place.containsKey(id)) place[id] = index }
                    by({ place[it.id] ?: Int.MAX_VALUE }, false)
                }
                ShellHomeSort.attention -> by({ attention(it) }, true)
                ShellHomeSort.count -> by({ entry -> entry.count?.let { maxOf(it, 0) } ?: if (entry.badge > 0) entry.badge else -1 }, true)
            }
        }

        fun attention(entry: ShellHomeEntry): Int {
            if (entry.badge > 0) return 2_000_000 + minOf(entry.badge, 999_999)
            if (entry.active) return 1_000_000
            return 0
        }

        fun rows(sections: List<ShellHomeArranged>, across: Int): Int {
            val width = maxOf(1, across)
            return sections.sumOf { (it.ids.size + width - 1) / width }
        }
    }
}
