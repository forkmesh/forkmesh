package com.forkmesh.nativeapp.codeinsight

import java.text.NumberFormat
import kotlin.math.ln1p

object Synapse {
    fun stableHash(text: String): Int {
        var hash = 2166136261L.toInt()
        for (ch in text) {
            hash = hash xor ch.code
            hash *= 16777619
        }
        return SynapseMath.mixHash(hash)
    }

    fun stableHashUnsigned(text: String): Long = stableHash(text).toLong() and 0xffffffffL

    fun fileName(path: String): String {
        val slash = path.lastIndexOf('/')
        return if (slash < 0) path else path.substring(slash + 1)
    }

    fun buildGraph(snapshot: InsightSnapshot, includeIsolated: Boolean): SynapseGraph =
        SynapseLayout.buildGraph(snapshot, includeIsolated)

    fun seedPositions(graph: SynapseGraph): FloatArray = SynapseLayout.seedPositions(graph)

    fun layoutIterations(graph: SynapseGraph, options: SynapseLayoutOptions = SynapseLayoutOptions()): Int =
        SynapseLayout.layoutIterations(graph, options)

    fun runLayout(
        graph: SynapseGraph,
        options: SynapseLayoutOptions = SynapseLayoutOptions(),
        publish: ((FloatArray, Double) -> Unit)? = null,
        stop: (() -> Boolean)? = null,
    ): FloatArray = SynapseLayout.runLayout(graph, options, publish, stop)

    fun lobeCentres(graph: SynapseGraph, positions: FloatArray): FloatArray = SynapseLayout.lobeCentres(graph, positions)

    fun importCount(snapshot: InsightSnapshot): Int {
        var total = 0
        for ((i, file) in snapshot.files.withIndex()) for (target in file.dependsOn) if (target != i) total++
        return total
    }

    fun cyclePath(snapshot: InsightSnapshot, members: List<Int>): List<Int> {
        if (members.isEmpty()) return emptyList()
        val inside = members.toHashSet()
        var start = members.first()
        for (member in members) {
            if (member >= 0 && member < snapshot.files.size && snapshot.files[member].path < snapshot.files[start].path) start = member
        }
        if (start < 0 || start >= snapshot.files.size) return emptyList()
        val parent = HashMap<Int, Int>()
        val queue = arrayListOf(start)
        parent[start] = -1
        var closing = -1
        var head = 0
        while (head < queue.size && closing < 0) {
            val current = queue[head++]
            val next = snapshot.files[current].dependsOn.sortedBy { snapshot.files[it].path }
            for (target in next) {
                if (target !in inside) continue
                if (target == start) {
                    closing = current
                    break
                }
                if (parent.containsKey(target)) continue
                parent[target] = current
                queue.add(target)
            }
        }
        if (closing < 0) return emptyList()
        val path = ArrayList<Int>()
        var at = closing
        while (at >= 0) {
            path.add(0, at)
            at = parent[at] ?: -1
        }
        path.add(start)
        return path
    }

    fun cycleText(snapshot: InsightSnapshot, path: List<Int>): String =
        path.filter { it >= 0 && it < snapshot.files.size }.joinToString(" → ") { fileName(snapshot.files[it].path) }

    fun cycleInfos(snapshot: InsightSnapshot): List<SynapseCycleInfo> =
        snapshot.cycles.mapIndexed { c, files -> SynapseCycleInfo(index = c, files = files, path = cyclePath(snapshot, files)) }

    private fun importsOther(file: InsightFile, self: Int): Boolean = file.dependsOn.any { it != self }

    private fun importedByOther(file: InsightFile, self: Int): Boolean = file.dependedOnBy.any { it != self }

    fun orphans(snapshot: InsightSnapshot): List<Int> = snapshot.files.indices
        .filter { !importsOther(snapshot.files[it], it) && !importedByOther(snapshot.files[it], it) }
        .sortedBy { snapshot.files[it].path }

    fun leaves(snapshot: InsightSnapshot): List<Int> = snapshot.files.indices
        .filter { !importsOther(snapshot.files[it], it) && importedByOther(snapshot.files[it], it) }

    private fun ranked(snapshot: InsightSnapshot, limit: Int, incoming: Boolean): List<Int> {
        val count = { i: Int -> if (incoming) snapshot.files[i].dependedOnBy.size else snapshot.files[i].dependsOn.size }
        val result = snapshot.files.indices.filter { count(it) > 0 }
            .sortedWith(compareByDescending<Int> { count(it) }.thenBy { snapshot.files[it].path })
        return if (limit >= 0 && result.size > limit) result.subList(0, limit) else result
    }

    fun mostImported(snapshot: InsightSnapshot, limit: Int): List<Int> = ranked(snapshot, limit, true)

    fun mostImporting(snapshot: InsightSnapshot, limit: Int): List<Int> = ranked(snapshot, limit, false)

    fun mostConnected(snapshot: InsightSnapshot): Int {
        var best = -1
        var bestScore = 0.0
        for ((i, file) in snapshot.files.withIndex()) {
            val incoming = file.dependedOnBy.size.toDouble()
            val outgoing = file.dependsOn.size.toDouble()
            val score = ln1p(incoming) * ln1p(outgoing) + 0.05 * ln1p(incoming + outgoing) + if (file.cycle >= 0) 0.5 else 0.0
            if (score > bestScore || (score == bestScore && best >= 0 && score > 0 && file.path < snapshot.files[best].path)) {
                best = i
                bestScore = score
            }
        }
        return best
    }

    fun neighbourhood(snapshot: InsightSnapshot, file: Int, hops: Int, direction: SynapseDirection): SynapseNeighbourhood {
        if (file < 0 || file >= snapshot.files.size || hops <= 0) return SynapseNeighbourhood()
        fun walk(forward: Boolean): Map<Int, Int> {
            val reached = LinkedHashMap<Int, Int>()
            var frontier = listOf(file)
            var depth = 1
            while (depth <= hops && frontier.isNotEmpty()) {
                val next = ArrayList<Int>()
                for (current in frontier) {
                    val info = snapshot.files[current]
                    for (target in if (forward) info.dependsOn else info.dependedOnBy) {
                        if (target == file || reached.containsKey(target) || target < 0 || target >= snapshot.files.size) continue
                        reached[target] = depth
                        next.add(target)
                    }
                }
                frontier = next
                depth++
            }
            return reached
        }
        return SynapseNeighbourhood(
            imports = if (direction != SynapseDirection.Importers) walk(true) else emptyMap(),
            importers = if (direction != SynapseDirection.Imports) walk(false) else emptyMap(),
        )
    }

    fun plural(count: Int, one: String, many: String, format: NumberFormat = NumberFormat.getIntegerInstance()): String =
        "${format.format(count)} ${if (count == 1) one else many}"

    fun summary(snapshot: InsightSnapshot, format: NumberFormat = NumberFormat.getIntegerInstance()): String =
        listOf(
            plural(importCount(snapshot), "import", "imports", format),
            plural(snapshot.cycles.size, "circular dependency", "circular dependencies", format),
            plural(orphans(snapshot).size, "orphan", "orphans", format),
        ).joinToString(" · ")
}
