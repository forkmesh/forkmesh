package com.forkmesh.nativeapp.codeinsight

import kotlin.math.abs
import kotlin.math.pow
import kotlin.math.sqrt

internal object Graphs {
    fun stronglyConnected(adjacency: List<List<Int>>): List<List<Int>> {
        val n = adjacency.size
        val index = IntArray(n) { -1 }
        val low = IntArray(n)
        val onStack = BooleanArray(n)
        val stack = ArrayList<Int>()
        val components = ArrayList<List<Int>>()
        var counter = 0
        val workNode = IntArray(n)
        val workEdge = IntArray(n)
        for (start in 0 until n) {
            if (index[start] >= 0) continue
            workNode[0] = start
            workEdge[0] = 0
            var depth = 1
            index[start] = counter
            low[start] = counter
            counter++
            stack.add(start)
            onStack[start] = true
            while (depth > 0) {
                val top = depth - 1
                val node = workNode[top]
                val edges = adjacency[node]
                if (workEdge[top] < edges.size) {
                    val next = edges[workEdge[top]++]
                    if (next < 0 || next >= n) continue
                    if (index[next] < 0) {
                        index[next] = counter
                        low[next] = counter
                        counter++
                        stack.add(next)
                        onStack[next] = true
                        workNode[depth] = next
                        workEdge[depth] = 0
                        depth++
                    } else if (onStack[next]) {
                        low[node] = minOf(low[node], index[next])
                    }
                    continue
                }
                depth--
                if (depth > 0) {
                    val parent = workNode[depth - 1]
                    low[parent] = minOf(low[parent], low[node])
                }
                if (low[node] == index[node]) {
                    val component = ArrayList<Int>()
                    while (true) {
                        val member = stack.removeAt(stack.size - 1)
                        onStack[member] = false
                        component.add(member)
                        if (member == node) break
                    }
                    component.sort()
                    components.add(component)
                }
            }
        }
        return components
    }

    fun markCycles(snapshot: InsightSnapshot) {
        val cycles = stronglyConnected(snapshot.files.map { it.dependsOn })
            .filter { it.size > 1 }
            .sortedWith(compareByDescending<List<Int>> { it.size }.thenBy { it.first() })
        for (file in snapshot.files) file.cycle = -1
        for ((c, cycle) in cycles.withIndex()) for (member in cycle) snapshot.files[member].cycle = c
        snapshot.cycles = cycles
    }

    fun applyWindows(snapshot: InsightSnapshot, significant: List<IntArray>, windows: List<LongArray>) {
        var total = 0L
        for (list in windows) total += list.size
        if (total > 6_000_000) return
        val all = LongArray(total.toInt())
        var at = 0
        for (list in windows) {
            list.copyInto(all, at)
            at += list.size
        }
        all.sort()
        fun repeated(hash: Long): Boolean {
            val found = all.binarySearch(hash)
            if (found < 0) return false
            return (found > 0 && all[found - 1] == hash) || (found + 1 < all.size && all[found + 1] == hash)
        }
        val files = snapshot.files
        for (i in 0 until minOf(files.size, windows.size)) {
            val list = windows[i]
            val lines = significant[i]
            val marked = BooleanArray(lines.size)
            var count = 0
            for (k in list.indices) {
                if (!repeated(list[k])) continue
                var j = 0
                while (j < Measure.WINDOW && k + j < lines.size) {
                    if (!marked[k + j]) {
                        marked[k + j] = true
                        count++
                    }
                    j++
                }
            }
            files[i].duplicatedLines = count
        }
    }

    fun computeHotspots(snapshot: InsightSnapshot) {
        var maxCommits = 0
        var maxLines = 1
        for (file in snapshot.files) {
            maxCommits = maxOf(maxCommits, file.commits)
            maxLines = maxOf(maxLines, file.codeLines)
        }
        var top = 0.0
        for (file in snapshot.files) {
            if (maxCommits <= 0 || file.commits <= 0) {
                file.hotspot = 0.0
                continue
            }
            val change = file.commits.toDouble() / maxCommits.toDouble()
            val size = sqrt(maxOf(0, file.codeLines).toDouble()) / sqrt(maxLines.toDouble())
            file.hotspot = change.pow(0.7) * maxOf(0.02, size).pow(0.3)
            top = maxOf(top, file.hotspot)
        }
        if (top > 0) for (file in snapshot.files) file.hotspot = (file.hotspot / top).coerceIn(0.0, 1.0)
    }

    fun modulePathFor(filePath: String, depth: Int): String {
        val segments = filePath.split('/')
        val dirs = segments.size - 1
        val take = minOf(maxOf(1, depth), dirs)
        if (take <= 0) return ""
        return segments.subList(0, take).joinToString("/")
    }

    fun autoModuleDepth(snapshot: InsightSnapshot): Int {
        var previousCount = 0
        var bestDepth = 1
        var bestCount = 0
        for (depth in 1..6) {
            val count = snapshot.files.mapTo(HashSet()) { modulePathFor(it.path, depth) }.size
            if (count in 8..60) return depth
            if (count > 60) return if (previousCount >= 3) depth - 1 else if (count <= 140) depth else maxOf(1, depth - 1)
            if (count > bestCount) {
                bestCount = count
                bestDepth = depth
            }
            if (count == previousCount && depth > 1) break
            previousCount = count
        }
        return maxOf(1, bestDepth)
    }

    fun buildModuleGraph(snapshot: InsightSnapshot, requested: Int): InsightModuleGraph {
        val depth = if (requested > 0) requested else autoModuleDepth(snapshot)
        val files = snapshot.files
        val paths = files.mapTo(HashSet()) { modulePathFor(it.path, depth) }.sorted()
        val byPath = HashMap<String, Int>()
        val nodes = paths.mapIndexed { index, path ->
            byPath[path] = index
            InsightModuleNode(path = path, name = path.ifEmpty { "(root files)" })
        }
        val nodeFiles = Array(nodes.size) { ArrayList<Int>() }
        val weightedHealth = DoubleArray(nodes.size)
        val weights = DoubleArray(nodes.size)
        val moduleOfFile = IntArray(files.size)
        for ((i, file) in files.withIndex()) {
            val module = byPath.getValue(modulePathFor(file.path, depth))
            moduleOfFile[i] = module
            val node = nodes[module]
            nodeFiles[module].add(i)
            node.codeLines += file.codeLines
            node.complexity += file.complexity
            node.functions += file.functions.size
            node.types += file.types
            node.abstractTypes += file.abstractTypes
            node.commits += file.commits
            val weight = maxOf(1, file.codeLines).toDouble()
            weightedHealth[module] += file.health * weight
            weights[module] += weight
        }
        val edgeIndex = HashMap<Long, Int>()
        val edges = ArrayList<InsightModuleEdge>()
        val edgeFiles = ArrayList<ArrayList<Pair<Int, Int>>>()
        val incomingFiles = Array(nodes.size) { HashSet<Int>() }
        val outgoingFiles = Array(nodes.size) { HashSet<Int>() }
        for ((i, file) in files.withIndex()) {
            val from = moduleOfFile[i]
            for (target in file.dependsOn) {
                val to = moduleOfFile[target]
                if (from == to) {
                    nodes[from].internalEdges++
                    continue
                }
                val key = (from.toLong() shl 32) or (to.toLong() and 0xffffffffL)
                val at = edgeIndex.getOrPut(key) {
                    edges.add(InsightModuleEdge(from = from, to = to))
                    edgeFiles.add(ArrayList())
                    edges.size - 1
                }
                edges[at].weight++
                edgeFiles[at].add(i to target)
                outgoingFiles[from].add(target)
                incomingFiles[to].add(i)
            }
        }
        for ((at, edge) in edges.withIndex()) edge.fileEdges = edgeFiles[at]
        val adjacency = List(nodes.size) { ArrayList<Int>() }
        for (edge in edges) adjacency[edge.from].add(edge.to)
        val components = stronglyConnected(adjacency)
        val componentOf = IntArray(nodes.size) { -1 }
        for ((c, component) in components.withIndex()) for (member in component) componentOf[member] = c
        val cycles = components.filter { it.size > 1 }.sortedByDescending { it.size }
        for ((c, cycle) in cycles.withIndex()) for (member in cycle) nodes[member].cycle = c
        for (edge in edges) edge.inCycle = componentOf[edge.from] == componentOf[edge.to]
        val componentCount = components.size
        val componentOut = Array(componentCount) { HashSet<Int>() }
        val componentIn = Array(componentCount) { HashSet<Int>() }
        for (edge in edges) {
            val a = componentOf[edge.from]
            val b = componentOf[edge.to]
            if (a != b) {
                componentOut[a].add(b)
                componentIn[b].add(a)
            }
        }
        val remaining = IntArray(componentCount)
        val layer = IntArray(componentCount)
        val queue = ArrayList<Int>()
        for (c in 0 until componentCount) {
            remaining[c] = componentOut[c].size
            if (remaining[c] == 0) queue.add(c)
        }
        var head = 0
        while (head < queue.size) {
            val c = queue[head++]
            for (predecessor in componentIn[c]) {
                layer[predecessor] = maxOf(layer[predecessor], layer[c] + 1)
                if (--remaining[predecessor] == 0) queue.add(predecessor)
            }
        }
        for ((m, node) in nodes.withIndex()) {
            node.files = nodeFiles[m]
            node.layer = layer[componentOf[m]]
            node.afferent = incomingFiles[m].size
            node.efferent = outgoingFiles[m].size
            val total = node.afferent + node.efferent
            node.instability = if (total > 0) node.efferent.toDouble() / total.toDouble() else 0.0
            node.abstractness = if (node.types > 0) node.abstractTypes.toDouble() / node.types.toDouble() else 0.0
            node.distance = abs(node.abstractness + node.instability - 1.0)
            node.cohesion = if (node.files.isEmpty()) 0.0 else (node.internalEdges + 1.0) / node.files.size.toDouble()
            node.health = if (weights[m] > 0) Health.roundHalfAway(weightedHealth[m] / weights[m] * 10.0) / 10.0 else 10.0
        }
        return InsightModuleGraph(depth, nodes, edges, cycles, moduleOfFile.toList())
    }
}
