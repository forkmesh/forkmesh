package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min

enum class ShellBarSort(override val raw: String) : ShellRaw {
    usual("usual"),
    recent("recent"),
    frequent("frequent");

    companion object {
        fun fromRaw(raw: String?): ShellBarSort? = entries.firstOrNull { it.raw == raw }
    }
}

class ShellBarOrder(private val store: PrefStore) {
    var sort: ShellBarSort
        get() {
            ShellBarSort.fromRaw(store.getString(SORT_KEY))?.let { return it }
            return if (store.getBool(RECENT_FIRST_KEY) == true) ShellBarSort.recent else ShellBarSort.usual
        }
        set(value) {
            store.putString(SORT_KEY, value.raw)
            store.remove(RECENT_FIRST_KEY)
        }

    val recent: List<String> get() = stringList(RECENT_KEY) ?: emptyList()

    val uses: Map<String, Int>
        get() {
            val lines = stringList(USES_KEY) ?: return recent.associateWithTo(LinkedHashMap()) { 1 }
            return decodeUses(lines)
        }

    val ranking: List<String> get() = ranking(sort, recent, uses)

    fun use(id: String) {
        val counts = uses
        val next = used(id, recent)
        putStringList(RECENT_KEY, next)
        putStringList(USES_KEY, encodeUses(counted(id, counts, next)))
    }

    fun visit(ids: List<String>): Boolean {
        val id = counted(ids, recent) ?: return false
        use(id)
        return true
    }

    fun clear() {
        putStringList(RECENT_KEY, emptyList())
        putStringList(USES_KEY, emptyList())
    }

    fun <Item> ordered(items: List<Item>, id: (Item) -> String): List<Item> = ordered(items, ranking, id)

    private fun stringList(key: String): List<String>? {
        val text = store.getString(key) ?: return null
        val value = try {
            Json.parse(text)
        } catch (_: Json.JsonException) {
            return null
        }
        return (value as? List<*>)?.filterIsInstance<String>()
    }

    private fun putStringList(key: String, value: List<String>) {
        store.putString(key, Json.stringify(value))
    }

    companion object {
        const val SORT_KEY = "shell/bar-sort"
        const val RECENT_FIRST_KEY = "shell/bar-recent-first"
        const val RECENT_KEY = "shell/bar-recent"
        const val USES_KEY = "shell/bar-uses"
        const val LIMIT = 48
        const val FREQUENT_MINIMUM_USES = 3
        const val FREQUENT_MINIMUM_HEAT = 0.5
        const val SEARCH_PULL = 56.0

        fun used(id: String, recent: List<String>): List<String> = (listOf(id) + recent.filter { it != id }).take(LIMIT)

        fun counted(id: String, uses: Map<String, Int>, recent: List<String>): Map<String, Int> {
            val next = LinkedHashMap(uses)
            next[id] = (next[id] ?: 0) + 1
            if (next.size <= LIMIT) return next
            val kept = listOf(id) + frequent(next, recent).filter { it != id }
            val trimmed = LinkedHashMap<String, Int>()
            for (key in kept.take(LIMIT)) trimmed[key] = next[key] ?: 0
            return trimmed
        }

        fun frequent(uses: Map<String, Int>, recent: List<String>): List<String> {
            val seen = HashMap<String, Int>()
            recent.forEachIndexed { index, key -> if (!seen.containsKey(key)) seen[key] = index }
            return uses.entries.filter { it.value > 0 }
                .sortedWith { a, b ->
                    if (a.value != b.value) {
                        b.value.compareTo(a.value)
                    } else {
                        val left = seen[a.key] ?: Int.MAX_VALUE
                        val right = seen[b.key] ?: Int.MAX_VALUE
                        if (left == right) a.key.compareTo(b.key) else left.compareTo(right)
                    }
                }
                .map { it.key }
        }

        fun mostUses(uses: Map<String, Int>, among: List<String>): Int = among.maxOfOrNull { uses[it] ?: 0 } ?: 0

        fun heat(id: String, uses: Map<String, Int>, among: List<String>): Double = heat(id, uses, mostUses(uses, among))

        fun heat(id: String, uses: Map<String, Int>, most: Int): Double {
            val count = uses[id] ?: 0
            if (count <= 0 || most <= 0) return 0.0
            return min(ln(1.0 + count) / ln(1.0 + most), 1.0)
        }

        fun counted(visit: List<String>, recent: List<String>): String? {
            val shown = visit.filter { it.isNotEmpty() }
            val first = shown.firstOrNull() ?: return null
            if (shown.any { it == recent.firstOrNull() }) return null
            return first
        }

        fun frequentMark(id: String, uses: Map<String, Int>, among: List<String>): Double? =
            frequentMark(id, uses, mostUses(uses, among))

        fun frequentMark(id: String, uses: Map<String, Int>, most: Int): Double? {
            if ((uses[id] ?: 0) < FREQUENT_MINIMUM_USES) return null
            val level = heat(id, uses, most)
            return if (level >= FREQUENT_MINIMUM_HEAT) level else null
        }

        fun revealsSearch(pulled: Double, shown: Boolean): Boolean = !shown && pulled >= SEARCH_PULL

        fun fittedTile(height: Double, fixed: Double, rows: Int, tile: Double, minimum: Double): Double {
            if (rows <= 0 || height <= 0) return tile
            return min(tile, max(min(minimum, tile), (height - fixed) / rows))
        }

        fun ranking(sort: ShellBarSort, recent: List<String>, uses: Map<String, Int>): List<String> = when (sort) {
            ShellBarSort.usual -> emptyList()
            ShellBarSort.recent -> recent
            ShellBarSort.frequent -> frequent(uses, recent)
        }

        fun encodeUses(uses: Map<String, Int>): List<String> =
            uses.entries.sortedBy { it.key }.map { "${it.value}:${it.key}" }

        fun decodeUses(lines: List<String>): Map<String, Int> {
            val uses = LinkedHashMap<String, Int>()
            for (line in lines) {
                val colon = line.indexOf(':')
                if (colon < 0) continue
                val count = line.substring(0, colon).toIntOrNull() ?: continue
                if (count <= 0) continue
                val id = line.substring(colon + 1)
                if (id.isNotEmpty()) uses[id] = count
            }
            return uses
        }

        fun <Item> ordered(items: List<Item>, recent: List<String>, id: (Item) -> String): List<Item> {
            if (recent.isEmpty()) return items
            val rank = HashMap<String, Int>()
            recent.forEachIndexed { index, key -> if (!rank.containsKey(key)) rank[key] = index }
            return items.sortedBy { rank[id(it)] ?: Int.MAX_VALUE }
        }
    }
}
