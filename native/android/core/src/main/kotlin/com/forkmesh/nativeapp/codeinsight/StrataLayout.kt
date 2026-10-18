package com.forkmesh.nativeapp.codeinsight

import kotlin.math.abs
import kotlin.math.log2
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

data class StrataPoint(val x: Double, val y: Double)

data class StrataSize(val width: Double, val height: Double)

data class StrataRect(val x: Double = 0.0, val y: Double = 0.0, val width: Double = 0.0, val height: Double = 0.0) {
    val left: Double get() = x
    val top: Double get() = y
    val right: Double get() = x + width
    val bottom: Double get() = y + height
    val centerX: Double get() = x + width / 2.0
    val centerY: Double get() = y + height / 2.0
    val isNull: Boolean get() = width == 0.0 && height == 0.0
    val isEmpty: Boolean get() = width <= 0.0 || height <= 0.0

    constructor(point: StrataPoint, size: StrataSize) : this(point.x, point.y, size.width, size.height)

    fun adjusted(x1: Double, y1: Double, x2: Double, y2: Double): StrataRect =
        StrataRect(x + x1, y + y1, width + x2 - x1, height + y2 - y1)

    fun contains(px: Double, py: Double): Boolean = px >= left && px <= right && py >= top && py <= bottom

    fun intersects(other: StrataRect): Boolean =
        !isEmpty && !other.isEmpty && left < other.right && other.left < right && top < other.bottom && other.top < bottom

    fun united(other: StrataRect): StrataRect {
        if (isNull) return other
        if (other.isNull) return this
        var l = x
        var r = x
        if (width < 0) l += width else r += width
        if (other.width < 0) {
            l = min(l, other.x + other.width)
            r = max(r, other.x)
        } else {
            l = min(l, other.x)
            r = max(r, other.x + other.width)
        }
        var t = y
        var b = y
        if (height < 0) t += height else b += height
        if (other.height < 0) {
            t = min(t, other.y + other.height)
            b = max(b, other.y)
        } else {
            t = min(t, other.y)
            b = max(b, other.y + other.height)
        }
        return StrataRect(l, t, r - l, b - t)
    }
}

data class StrataLink(val from: Int, val to: Int, val weight: Int = 1, val cycle: Boolean = false)

class StrataLayoutInput(
    val sizes: List<StrataSize>,
    val links: List<StrataLink> = emptyList(),
    val layers: List<Int> = emptyList(),
    val layerGap: Double = 96.0,
    val nodeGap: Double = 40.0,
    val maxPerRank: Int = 0,
)

data class StrataRoutedEdge(
    val from: Int,
    val to: Int,
    val weight: Int,
    val cycle: Boolean,
    val flat: Boolean,
    val points: List<StrataPoint>,
)

data class StrataGraphLayout(
    val boxes: List<StrataRect> = emptyList(),
    val ranks: List<Int> = emptyList(),
    val edges: List<StrataRoutedEdge> = emptyList(),
    val bounds: StrataRect = StrataRect(),
    val crossings: Int = 0,
    val rankCount: Int = 0,
)

data class StrataDrillNode(val module: Boolean, val dependent: Boolean, val index: Int, val weight: Int)

class StrataDrillGraph(
    val module: Int,
    val nodes: List<StrataDrillNode>,
    val links: List<StrataLink>,
    val layout: StrataGraphLayout,
    val hiddenFiles: Int,
)

enum class StrataTreemapSize(val label: String) {
    Lines("Lines of code"),
    Complexity("Complexity"),
    Functions("Functions"),
}

class StrataTreemap(val modules: List<StrataRect>, val files: List<StrataRect>)

object StrataLayout {
    private class Chain(val link: Int) {
        var reversed = false
        var flat = false
        val nodes = ArrayList<Int>()
    }

    private class Block(val sum: Double, val weight: Double, val count: Int)

    private fun isotonic(values: DoubleArray, weights: DoubleArray) {
        val blocks = ArrayList<Block>(values.size)
        for (i in values.indices) {
            val w = max(weights[i], 1e-3)
            blocks.add(Block(values[i] * w, w, 1))
            while (blocks.size >= 2) {
                val b = blocks[blocks.size - 1]
                val a = blocks[blocks.size - 2]
                if (a.sum / a.weight <= b.sum / b.weight) break
                val merged = Block(a.sum + b.sum, a.weight + b.weight, a.count + b.count)
                blocks.removeAt(blocks.size - 1)
                blocks[blocks.size - 1] = merged
            }
        }
        var i = 0
        for (b in blocks) {
            val mean = b.sum / b.weight
            repeat(b.count) { values[i++] = mean }
        }
    }

    fun layeredLayout(input: StrataLayoutInput): StrataGraphLayout {
        val n = input.sizes.size
        val boxes = Array(n) { StrataRect() }
        val outRanks = IntArray(n) { -1 }
        if (n == 0) return StrataGraphLayout()

        val links = ArrayList<StrataLink>()
        val seen = HashSet<Long>()
        val adjacency = List(n) { ArrayList<Int>() }
        val degree = IntArray(n)
        for (link in input.links) {
            if (link.from < 0 || link.from >= n || link.to < 0 || link.to >= n || link.from == link.to) continue
            val key = link.from.toLong() * n + link.to
            if (!seen.add(key)) continue
            links.add(link)
            adjacency[link.from].add(link.to)
            degree[link.from]++
            degree[link.to]++
        }
        val parts = Strata.components(adjacency)
        val comp = IntArray(n) { -1 }
        val compSize = IntArray(parts.size)
        for ((c, part) in parts.withIndex()) {
            compSize[c] = part.size
            for (v in part) comp[v] = c
        }
        val layer = IntArray(n)
        if (input.layers.size == n) {
            for (i in 0 until n) layer[i] = max(0, input.layers[i])
        } else {
            val compLayer = IntArray(parts.size)
            for ((c, part) in parts.withIndex()) {
                for (v in part) for (w in adjacency[v]) {
                    if (comp[w] != c) compLayer[c] = max(compLayer[c], compLayer[comp[w]] + 1)
                }
            }
            for (i in 0 until n) layer[i] = compLayer[comp[i]]
        }

        val active = ArrayList<Int>()
        val isolated = ArrayList<Int>()
        for (i in 0 until n) (if (degree[i] > 0) active else isolated).add(i)

        val sub = IntArray(n)
        run {
            val weightOf = HashMap<Long, Int>()
            for (link in links) weightOf[link.from.toLong() * n + link.to] = max(1, link.weight)
            fun weight(a: Int, b: Int): Double = (weightOf[a.toLong() * n + b] ?: 1).toDouble()
            for (part in parts) {
                if (part.size < 2) continue
                val members = part.toHashSet()
                val remaining = HashSet(members)
                val outWeight = HashMap<Int, Double>()
                val inWeight = HashMap<Int, Double>()
                val inside = HashMap<Int, ArrayList<Int>>()
                for (v in part) {
                    for (w in adjacency[v]) {
                        if (w !in members) continue
                        val wt = weight(v, w)
                        outWeight[v] = (outWeight[v] ?: 0.0) + wt
                        inWeight[w] = (inWeight[w] ?: 0.0) + wt
                        inside.getOrPut(v) { ArrayList() }.add(w)
                    }
                }
                val incoming = HashMap<Int, ArrayList<Int>>()
                for (v in part) for (w in inside[v].orEmpty()) incoming.getOrPut(w) { ArrayList() }.add(v)
                fun remove(v: Int) {
                    remaining.remove(v)
                    for (w in inside[v].orEmpty()) {
                        if (w in remaining) inWeight[w] = (inWeight[w] ?: 0.0) - weight(v, w)
                    }
                    for (u in incoming[v].orEmpty()) {
                        if (u in remaining) outWeight[u] = (outWeight[u] ?: 0.0) - weight(u, v)
                    }
                }
                val front = ArrayList<Int>()
                val back = ArrayList<Int>()
                while (remaining.isNotEmpty()) {
                    var changed = true
                    while (changed) {
                        changed = false
                        for (v in part) {
                            if (v in remaining && (outWeight[v] ?: 0.0) <= 1e-9) {
                                back.add(0, v)
                                remove(v)
                                changed = true
                            }
                        }
                        for (v in part) {
                            if (v in remaining && (inWeight[v] ?: 0.0) <= 1e-9) {
                                front.add(v)
                                remove(v)
                                changed = true
                            }
                        }
                    }
                    if (remaining.isEmpty()) break
                    var best = -1
                    var bestDelta = -1e18
                    for (v in part) {
                        if (v !in remaining) continue
                        val delta = (outWeight[v] ?: 0.0) - (inWeight[v] ?: 0.0)
                        if (delta > bestDelta) {
                            bestDelta = delta
                            best = v
                        }
                    }
                    front.add(best)
                    remove(best)
                }
                val sequence = front + back
                val position = HashMap<Int, Int>()
                for ((i, v) in sequence.withIndex()) position[v] = i
                for (v in sequence) {
                    for (u in incoming[v].orEmpty()) {
                        if ((position[u] ?: 0) < (position[v] ?: 0)) sub[v] = max(sub[v], sub[u] + 1)
                    }
                }
            }
        }

        val rank = IntArray(n) { -1 }
        run {
            var maxLayer = 0
            var maxSub = 0
            for (v in active) {
                maxLayer = max(maxLayer, layer[v])
                maxSub = max(maxSub, sub[v])
            }
            val band = maxSub + 1L
            val distinct = active.map { (maxLayer - layer[it]).toLong() * band + sub[it] }.distinct().sorted()
            val rankOf = HashMap<Long, Int>()
            for ((i, key) in distinct.withIndex()) rankOf[key] = i
            for (v in active) rank[v] = rankOf[(maxLayer - layer[v]).toLong() * band + sub[v]] ?: 0
        }
        var rankCount = 0
        for (v in active) rankCount = max(rankCount, rank[v] + 1)

        var cap = input.maxPerRank
        if (cap <= 0) {
            var widths = 0.0
            var heights = 0.0
            for (v in active) {
                widths += input.sizes[v].width + input.nodeGap
                heights += input.sizes[v].height + input.layerGap
            }
            val count = max(1, active.size).toDouble()
            val aspect = 1.5
            cap = max(4, Math.round(sqrt(aspect * count * (heights / count) / max(1.0, widths / count))).toInt())
        }
        val predecessors = List(n) { ArrayList<Int>() }
        for (link in links) predecessors[link.to].add(link.from)
        run {
            val maxRank = rankCount - 1
            val sequence = active.sortedWith { a, b ->
                val la = maxRank - rank[a]
                val lb = maxRank - rank[b]
                when {
                    la != lb -> la.compareTo(lb)
                    predecessors[a].size != predecessors[b].size -> predecessors[b].size.compareTo(predecessors[a].size)
                    else -> a.compareTo(b)
                }
            }
            val level = IntArray(n) { -1 }
            val fill = HashMap<Int, Int>()
            var top = 0
            for (v in sequence) {
                var l = maxRank - rank[v]
                for (w in adjacency[v]) {
                    if (rank[w] > rank[v] && level[w] >= 0) l = max(l, level[w] + 1)
                }
                while ((fill[l] ?: 0) >= cap) l++
                level[v] = l
                fill[l] = (fill[l] ?: 0) + 1
                top = max(top, l)
            }
            for (v in active) rank[v] = top - level[v]
            rankCount = top + 1
        }
        run {
            val used = BooleanArray(rankCount)
            for (v in active) used[rank[v]] = true
            val remap = IntArray(rankCount) { -1 }
            var next = 0
            for (r in 0 until rankCount) if (used[r]) remap[r] = next++
            for (v in active) rank[v] = remap[rank[v]]
            rankCount = next
        }

        val vrank = ArrayList<Int>(n)
        val vwidth = ArrayList<Double>(n)
        for (i in 0 until n) {
            vrank.add(rank[i])
            vwidth.add(input.sizes[i].width)
        }
        val dummyWidth = 6.0
        val down = ArrayList<ArrayList<Int>>(n)
        val up = ArrayList<ArrayList<Int>>(n)
        repeat(n) {
            down.add(ArrayList())
            up.add(ArrayList())
        }
        val chains = ArrayList<Chain>()
        var dummies = 0
        for ((l, link) in links.withIndex()) {
            val chain = Chain(l)
            val ru = rank[link.from]
            val rv = rank[link.to]
            if (ru == rv) {
                chain.flat = true
                chain.nodes.add(link.from)
                chain.nodes.add(link.to)
                chains.add(chain)
                continue
            }
            chain.reversed = ru > rv
            val top = if (chain.reversed) link.to else link.from
            val bottom = if (chain.reversed) link.from else link.to
            chain.nodes.add(top)
            val span = rank[bottom] - rank[top] - 1
            if (dummies + span <= 30000) {
                for (r in rank[top] + 1 until rank[bottom]) {
                    val d = vrank.size
                    vrank.add(r)
                    vwidth.add(dummyWidth)
                    down.add(ArrayList())
                    up.add(ArrayList())
                    chain.nodes.add(d)
                    dummies++
                }
            }
            chain.nodes.add(bottom)
            for (k in 0 until chain.nodes.size - 1) {
                down[chain.nodes[k]].add(chain.nodes[k + 1])
                up[chain.nodes[k + 1]].add(chain.nodes[k])
            }
            chains.add(chain)
        }
        val total = vrank.size
        fun isReal(v: Int) = v < n

        var order: MutableList<MutableList<Int>> = MutableList(rankCount) { ArrayList() }
        for (v in 0 until total) if (vrank[v] >= 0) order[vrank[v]].add(v)
        val pos = DoubleArray(total)
        fun refreshPositions() {
            for (row in order) for (i in row.indices) pos[row[i]] = i.toDouble()
        }
        fun groupOf(v: Int): Int = if (isReal(v) && compSize[comp[v]] > 1) comp[v] else -1
        if (order.isNotEmpty()) {
            order[0] = order[0].sortedWith(compareBy<Int>({ comp[it] }, { it })).toMutableList()
        }
        refreshPositions()

        class Key(val unit: Double, val tag: Int, val own: Double, val current: Double, val v: Int)

        fun sweep(r: Int, fromAbove: Boolean) {
            val row = order[r]
            val count = row.size
            val own = DoubleArray(count)
            for (i in 0 until count) {
                val neighbours = if (fromAbove) up[row[i]] else down[row[i]]
                if (neighbours.isEmpty()) {
                    own[i] = pos[row[i]]
                    continue
                }
                var sum = 0.0
                for (w in neighbours) sum += pos[w]
                own[i] = sum / neighbours.size
            }
            val unitSum = HashMap<Int, Double>()
            val unitCount = HashMap<Int, Int>()
            for (i in 0 until count) {
                val g = groupOf(row[i])
                if (g < 0) continue
                unitSum[g] = (unitSum[g] ?: 0.0) + own[i]
                unitCount[g] = (unitCount[g] ?: 0) + 1
            }
            val keys = ArrayList<Key>(count)
            for (i in 0 until count) {
                val v = row[i]
                val g = groupOf(v)
                val unitKey = if (g >= 0) (unitSum[g] ?: 0.0) / (unitCount[g] ?: 1) else own[i]
                keys.add(Key(unitKey, if (g >= 0) g else n + v, own[i], pos[v], v))
            }
            val sorted = keys.sortedWith { a, b ->
                when {
                    a.unit != b.unit -> if (a.unit < b.unit) -1 else 1
                    a.tag != b.tag -> a.tag.compareTo(b.tag)
                    a.own != b.own -> if (a.own < b.own) -1 else 1
                    a.current != b.current -> if (a.current < b.current) -1 else 1
                    else -> 0
                }
            }
            for (i in 0 until count) {
                row[i] = sorted[i].v
                pos[row[i]] = i.toDouble()
            }
        }

        fun countCrossings(): Int {
            var crossings = 0
            for (r in 0 until rankCount - 1) {
                val edges = ArrayList<Long>()
                for (v in order[r]) {
                    for (w in down[v]) {
                        if (vrank[w] == r + 1) edges.add((pos[v].toInt().toLong() shl 32) or pos[w].toInt().toLong())
                    }
                }
                edges.sort()
                val width = order[r + 1].size + 1
                val tree = IntArray(width + 1)
                var seenCount = 0
                for (edge in edges) {
                    val second = (edge and 0xffffffffL).toInt()
                    var greater = seenCount
                    var i = second + 1
                    while (i > 0) {
                        greater -= tree[i]
                        i -= i and -i
                    }
                    crossings += greater
                    i = second + 1
                    while (i <= width) {
                        tree[i] += 1
                        i += i and -i
                    }
                    seenCount++
                }
            }
            return crossings
        }

        fun copyOrder(): MutableList<MutableList<Int>> = order.map { ArrayList(it) as MutableList<Int> }.toMutableList()

        for (r in 1 until rankCount) sweep(r, true)
        var best = copyOrder()
        var bestCrossings = countCrossings()
        var iteration = 0
        while (iteration < 16 && bestCrossings > 0) {
            if (iteration % 2 == 0) {
                for (r in 1 until rankCount) sweep(r, true)
            } else {
                for (r in rankCount - 2 downTo 0) sweep(r, false)
            }
            val crossings = countCrossings()
            if (crossings < bestCrossings) {
                bestCrossings = crossings
                best = copyOrder()
            }
            iteration++
        }
        order = best
        refreshPositions()

        val x = DoubleArray(total)
        fun separation(a: Int, b: Int): Double {
            val ra = isReal(a)
            val rb = isReal(b)
            val gap = if (ra && rb) input.nodeGap else if (ra || rb) input.nodeGap * 0.45 else 10.0
            return (vwidth[a] + vwidth[b]) / 2.0 + gap
        }
        for (row in order) {
            var cursor = 0.0
            for (i in row.indices) {
                if (i > 0) cursor += separation(row[i - 1], row[i])
                x[row[i]] = cursor
            }
        }
        fun place(r: Int, mode: Int) {
            val row = order[r]
            val count = row.size
            if (count == 0) return
            val offsets = DoubleArray(count)
            for (i in 1 until count) offsets[i] = offsets[i - 1] + separation(row[i - 1], row[i])
            val targets = DoubleArray(count)
            val weights = DoubleArray(count)
            for (i in 0 until count) {
                val v = row[i]
                var sum = 0.0
                var k = 0
                if (mode != 1) for (w in up[v]) {
                    sum += x[w]
                    k++
                }
                if (mode != 0) for (w in down[v]) {
                    sum += x[w]
                    k++
                }
                val desired = if (k != 0) sum / k else x[v]
                targets[i] = desired - offsets[i]
                weights[i] = if (k == 0) 0.05 else if (isReal(v)) 1.0 + 0.2 * k else 6.0
            }
            isotonic(targets, weights)
            for (i in 0 until count) x[row[i]] = targets[i] + offsets[i]
        }
        repeat(10) {
            for (r in 1 until rankCount) place(r, 0)
            for (r in rankCount - 2 downTo 0) place(r, 1)
        }
        for (r in 0 until rankCount) place(r, 2)

        val rankTop = DoubleArray(rankCount)
        val rankHeight = DoubleArray(rankCount) { 20.0 }
        for (v in active) rankHeight[rank[v]] = max(rankHeight[rank[v]], input.sizes[v].height)
        var cursorY = 0.0
        for (r in 0 until rankCount) {
            rankTop[r] = cursorY
            cursorY += rankHeight[r] + input.layerGap
        }
        var minX = Double.MAX_VALUE
        for (v in 0 until total) if (vrank[v] >= 0) minX = min(minX, x[v] - vwidth[v] / 2.0)
        if (minX == Double.MAX_VALUE) minX = 0.0
        for (v in 0 until total) x[v] -= minX
        for (v in active) {
            val size = input.sizes[v]
            val r = rank[v]
            boxes[v] = StrataRect(x[v] - size.width / 2.0, rankTop[r] + (rankHeight[r] - size.height) / 2.0, size.width, size.height)
            outRanks[v] = r
        }

        val exitPort = HashMap<Long, Double>()
        val entryPort = HashMap<Long, Double>()
        run {
            val exits = List(n) { ArrayList<Pair<Double, Int>>() }
            val entries = List(n) { ArrayList<Pair<Double, Int>>() }
            for ((c, chain) in chains.withIndex()) {
                if (chain.flat) continue
                val top = chain.nodes.first()
                val bottom = chain.nodes.last()
                exits[top].add(x[chain.nodes[1]] to c)
                entries[bottom].add(x[chain.nodes[chain.nodes.size - 2]] to c)
            }
            val pairOrder = Comparator<Pair<Double, Int>> { a, b ->
                when {
                    a.first < b.first -> -1
                    b.first < a.first -> 1
                    else -> a.second.compareTo(b.second)
                }
            }
            for (v in 0 until n) {
                val box = boxes[v]
                fun assign(list: List<Pair<Double, Int>>, ports: HashMap<Long, Double>) {
                    val sorted = list.sortedWith(pairOrder)
                    val m = sorted.size
                    for (k in 0 until m) {
                        val t = if (m == 1) 0.5 else 0.22 + 0.56 * k.toDouble() / (m - 1)
                        ports[sorted[k].second.toLong() * n + v] = box.left + box.width * t
                    }
                }
                assign(exits[v], exitPort)
                assign(entries[v], entryPort)
            }
        }
        val edges = ArrayList<StrataRoutedEdge>()
        for ((c, chain) in chains.withIndex()) {
            val link = links[chain.link]
            val points: List<StrataPoint>
            if (chain.flat) {
                val a = boxes[link.from]
                val b = boxes[link.to]
                val below = a.centerX <= b.centerX
                val dx = abs(b.centerX - a.centerX)
                val sx = a.centerX + (if (below) 0.18 else -0.18) * a.width
                val ex = b.centerX + (if (below) -0.18 else 0.18) * b.width
                val sy = if (below) a.bottom else a.top
                val ey = if (below) b.bottom else b.top
                val bulge = 26 + 0.12 * dx
                val my = if (below) max(sy, ey) + bulge else min(sy, ey) - bulge
                points = listOf(StrataPoint(sx, sy), StrataPoint((sx + ex) / 2.0, my), StrataPoint(ex, ey))
            } else {
                val top = chain.nodes.first()
                val bottom = chain.nodes.last()
                val list = ArrayList<StrataPoint>()
                list.add(StrataPoint(exitPort[c.toLong() * n + top] ?: boxes[top].centerX, boxes[top].bottom))
                for (k in 1 until chain.nodes.size - 1) {
                    val d = chain.nodes[k]
                    val r = vrank[d]
                    list.add(StrataPoint(x[d], rankTop[r]))
                    list.add(StrataPoint(x[d], rankTop[r] + rankHeight[r]))
                }
                list.add(StrataPoint(entryPort[c.toLong() * n + bottom] ?: boxes[bottom].centerX, boxes[bottom].top))
                if (chain.reversed) list.reverse()
                points = list
            }
            edges.add(StrataRoutedEdge(link.from, link.to, link.weight, link.cycle, chain.flat, points))
        }

        var bounds = StrataRect()
        for (v in active) bounds = bounds.united(boxes[v])
        for (edge in edges) for (p in edge.points) bounds = bounds.united(StrataRect(p.x, p.y, 1.0, 1.0))
        if (isolated.isNotEmpty()) {
            var area = 0.0
            for (v in isolated) {
                area += (input.sizes[v].width + input.nodeGap * 0.6) * (input.sizes[v].height + input.nodeGap * 0.6)
            }
            val beside = active.isNotEmpty() && bounds.height > bounds.width * 1.1
            if (beside) {
                val startX = bounds.right + input.nodeGap * 2.0
                val limit = max(bounds.height, 400.0)
                var cx = startX
                var cy = bounds.top
                var columnWidth = 0.0
                for (v in isolated) {
                    val size = input.sizes[v]
                    if (cy > bounds.top && cy + size.height > bounds.top + limit) {
                        cy = bounds.top
                        cx += columnWidth + input.nodeGap * 0.6
                        columnWidth = 0.0
                    }
                    boxes[v] = StrataRect(cx, cy, size.width, size.height)
                    outRanks[v] = rankCount
                    cy += size.height + input.nodeGap * 0.6
                    columnWidth = max(columnWidth, size.width)
                }
            } else {
                val startY = if (active.isEmpty()) 0.0 else bounds.bottom + input.layerGap * 0.7
                val limit = (sqrt(area) * 1.9).coerceIn(600.0, max(if (active.isEmpty()) 0.0 else bounds.width, 1100.0))
                var cx = if (active.isEmpty()) 0.0 else bounds.left
                val left = cx
                var cy = startY
                var rowHeight = 0.0
                for (v in isolated) {
                    val size = input.sizes[v]
                    if (cx > left && cx + size.width > left + limit) {
                        cx = left
                        cy += rowHeight + input.nodeGap * 0.6
                        rowHeight = 0.0
                    }
                    boxes[v] = StrataRect(cx, cy, size.width, size.height)
                    outRanks[v] = rankCount
                    cx += size.width + input.nodeGap * 0.6
                    rowHeight = max(rowHeight, size.height)
                }
            }
            for (v in isolated) bounds = bounds.united(boxes[v])
            rankCount++
        }
        return StrataGraphLayout(boxes.toList(), outRanks.toList(), edges, bounds, bestCrossings, rankCount)
    }

    fun moduleBoxSize(codeLines: Int): StrataSize {
        val width = (96.0 + sqrt(max(0, codeLines).toDouble()) * 1.0).coerceIn(128.0, 340.0)
        val height = (width * 0.62).coerceIn(76.0, 210.0)
        return StrataSize(Strata.roundHalfAway(width), Strata.roundHalfAway(height))
    }

    fun moduleLayers(graph: InsightModuleGraph): List<Int> {
        val n = graph.nodes.size
        if (graph.nodes.any { it.layer > 0 }) {
            val layers = graph.nodes.map { it.layer }
            val consistent = graph.edges.none { !it.inCycle && it.from >= 0 && it.to >= 0 && layers[it.from] <= layers[it.to] }
            if (consistent) return layers
        }
        val adjacency = List(n) { ArrayList<Int>() }
        for (edge in graph.edges) {
            if (edge.from in 0 until n && edge.to in 0 until n && edge.from != edge.to) adjacency[edge.from].add(edge.to)
        }
        val parts = Strata.components(adjacency)
        val comp = IntArray(n)
        for ((c, part) in parts.withIndex()) for (v in part) comp[v] = c
        val compLayer = IntArray(parts.size)
        for ((c, part) in parts.withIndex()) {
            for (v in part) for (w in adjacency[v]) {
                if (comp[w] != c) compLayer[c] = max(compLayer[c], compLayer[comp[w]] + 1)
            }
        }
        return List(n) { compLayer[comp[it]] }
    }

    fun moduleLayout(graph: InsightModuleGraph): StrataGraphLayout = layeredLayout(
        StrataLayoutInput(
            sizes = graph.nodes.map { moduleBoxSize(it.codeLines) },
            links = graph.edges.map { StrataLink(it.from, it.to, it.weight, it.inCycle) },
            layers = moduleLayers(graph),
            layerGap = 80.0,
            nodeGap = 44.0,
        ),
    )

    fun drillGraph(snapshot: InsightSnapshot, graph: InsightModuleGraph, module: Int, maxFiles: Int = 120): StrataDrillGraph {
        if (module < 0 || module >= graph.nodes.size) {
            return StrataDrillGraph(module, emptyList(), emptyList(), StrataGraphLayout(), 0)
        }
        val all = snapshot.files
        fun degreeOf(f: Int) = all[f].dependsOn.size + all[f].dependedOnBy.size
        var files = graph.nodes[module].files.sortedWith { a, b ->
            val da = degreeOf(a)
            val db = degreeOf(b)
            if ((da > 0) != (db > 0)) {
                if (da > 0) -1 else 1
            } else {
                (all[b].codeLines + 40 * db).compareTo(all[a].codeLines + 40 * da)
            }
        }
        var hidden = 0
        if (files.size > maxFiles) {
            hidden = files.size - maxFiles
            files = files.subList(0, maxFiles)
        }
        val nodes = ArrayList<StrataDrillNode>()
        val nodeOfFile = HashMap<Int, Int>()
        for (f in files) {
            nodeOfFile[f] = nodes.size
            nodes.add(StrataDrillNode(false, false, f, all[f].codeLines))
        }
        fun moduleOf(file: Int) = if (file >= 0 && file < graph.moduleOfFile.size) graph.moduleOfFile[file] else -1
        val moduleCount = graph.nodes.size
        val usesWeight = HashMap<Int, Int>()
        val usedByWeight = HashMap<Int, Int>()
        val fileToModule = HashMap<Long, Int>()
        val moduleToFile = HashMap<Long, Int>()
        val internal = ArrayList<StrataLink>()
        for (f in files) {
            val file = all[f]
            for (d in file.dependsOn) {
                val m = moduleOf(d)
                if (m == module) {
                    val target = nodeOfFile[d]
                    if (target != null && d != f) internal.add(StrataLink(nodeOfFile.getValue(f), target, 1, false))
                } else if (m >= 0) {
                    usesWeight[m] = (usesWeight[m] ?: 0) + 1
                    val key = f.toLong() * moduleCount + m
                    fileToModule[key] = (fileToModule[key] ?: 0) + 1
                }
            }
            for (s in file.dependedOnBy) {
                val m = moduleOf(s)
                if (m >= 0 && m != module) {
                    usedByWeight[m] = (usedByWeight[m] ?: 0) + 1
                    val key = f.toLong() * moduleCount + m
                    moduleToFile[key] = (moduleToFile[key] ?: 0) + 1
                }
            }
        }
        fun strongest(weights: Map<Int, Int>, limit: Int): List<Int> = weights.entries
            .sortedWith(compareBy<Map.Entry<Int, Int>>({ -it.value }, { it.key }))
            .take(limit)
            .map { it.key }
        val usesNode = HashMap<Int, Int>()
        for (m in strongest(usesWeight, 24)) {
            usesNode[m] = nodes.size
            nodes.add(StrataDrillNode(true, false, m, usesWeight.getValue(m)))
        }
        val usedByNode = HashMap<Int, Int>()
        for (m in strongest(usedByWeight, 24)) {
            usedByNode[m] = nodes.size
            nodes.add(StrataDrillNode(true, true, m, usedByWeight.getValue(m)))
        }
        val raw = ArrayList(internal)
        for ((key, value) in fileToModule) {
            val f = (key / moduleCount).toInt()
            val m = (key % moduleCount).toInt()
            val target = usesNode[m] ?: continue
            raw.add(StrataLink(nodeOfFile.getValue(f), target, value, false))
        }
        for ((key, value) in moduleToFile) {
            val f = (key / moduleCount).toInt()
            val m = (key % moduleCount).toInt()
            val source = usedByNode[m] ?: continue
            raw.add(StrataLink(source, nodeOfFile.getValue(f), value, false))
        }
        val sortedLinks = raw.sortedWith(compareBy<StrataLink>({ it.from }, { it.to }))
        val adjacency = List(nodes.size) { ArrayList<Int>() }
        for (link in sortedLinks) adjacency[link.from].add(link.to)
        val cyclic = HashSet<Long>()
        for (part in Strata.components(adjacency)) {
            if (part.size < 2) continue
            val members = part.toHashSet()
            for (v in part) for (w in adjacency[v]) if (w in members) cyclic.add(v.toLong() * nodes.size + w)
        }
        val links = sortedLinks.map { it.copy(cycle = (it.from.toLong() * nodes.size + it.to) in cyclic) }
        val sizes = nodes.map { node ->
            if (node.module) {
                val name = graph.nodes[node.index].name
                StrataSize((60.0 + name.length * 7.0).coerceIn(120.0, 230.0), 36.0)
            } else {
                val name = Strata.fileName(all[node.index].path)
                val byName = 36.0 + name.length * 7.0
                val bySize = 80.0 + sqrt(max(0, node.weight).toDouble()) * 1.6
                StrataSize(max(byName, bySize).coerceIn(110.0, 260.0), 48.0)
            }
        }
        val layout = layeredLayout(StrataLayoutInput(sizes = sizes, links = links, layerGap = 84.0, nodeGap = 28.0))
        return StrataDrillGraph(module, nodes, links, layout, hidden)
    }

    fun matrixOrder(graph: InsightModuleGraph): List<Int> {
        val n = graph.nodes.size
        val layers = moduleLayers(graph)
        val cycleLayer = HashMap<Int, Int>()
        val cycleName = HashMap<Int, String>()
        for (i in 0 until n) {
            val c = graph.nodes[i].cycle
            if (c < 0) continue
            cycleLayer[c] = max(cycleLayer[c] ?: 0, layers[i])
            val current = cycleName[c].orEmpty()
            if (current.isEmpty() || graph.nodes[i].path < current) cycleName[c] = graph.nodes[i].path
        }
        return (0 until n).sortedWith { a, b ->
            val na = graph.nodes[a]
            val nb = graph.nodes[b]
            val la = if (na.cycle >= 0) cycleLayer[na.cycle] ?: 0 else layers[a]
            val lb = if (nb.cycle >= 0) cycleLayer[nb.cycle] ?: 0 else layers[b]
            val ga = if (na.cycle >= 0) cycleName[na.cycle].orEmpty() else na.path
            val gb = if (nb.cycle >= 0) cycleName[nb.cycle].orEmpty() else nb.path
            val ca = if (na.cycle >= 0) 0 else 1
            val cb = if (nb.cycle >= 0) 0 else 1
            when {
                la != lb -> lb.compareTo(la)
                ga != gb -> ga.compareTo(gb)
                ca != cb -> ca.compareTo(cb)
                else -> na.path.compareTo(nb.path)
            }
        }
    }

    fun squarify(weights: List<Double>, rect: StrataRect): List<StrataRect> {
        val out = Array(weights.size) { StrataRect() }
        val positive = ArrayList<Int>()
        var total = 0.0
        for (i in weights.indices) {
            if (weights[i] > 0) {
                positive.add(i)
                total += weights[i]
            }
        }
        if (positive.isEmpty() || total <= 0 || rect.width <= 0 || rect.height <= 0) return out.toList()
        val order = positive.sortedWith { a, b ->
            when {
                weights[a] > weights[b] -> -1
                weights[b] > weights[a] -> 1
                else -> 0
            }
        }
        val scale = rect.width * rect.height / total
        var fx = rect.x
        var fy = rect.y
        var fw = rect.width
        var fh = rect.height
        var i = 0
        while (i < order.size) {
            val side = min(fw, fh)
            if (side <= 0) {
                while (i < order.size) {
                    out[order[i]] = StrataRect(fx, fy, 0.0, 0.0)
                    i++
                }
                break
            }
            var rowSum = 0.0
            var minArea = Double.MAX_VALUE
            var maxArea = 0.0
            var worst = Double.MAX_VALUE
            var j = i
            while (j < order.size) {
                val area = weights[order[j]] * scale
                val sum = rowSum + area
                val lo = min(minArea, area)
                val hi = max(maxArea, area)
                val s2 = side * side
                val candidate = max(s2 * hi / (sum * sum), (sum * sum) / (s2 * lo))
                if (j > i && candidate > worst) break
                worst = candidate
                rowSum = sum
                minArea = lo
                maxArea = hi
                j++
            }
            val thickness = rowSum / side
            if (fw >= fh) {
                var y = fy
                for (k in i until j) {
                    val h = weights[order[k]] * scale / thickness
                    out[order[k]] = StrataRect(fx, y, thickness, if (k + 1 == j) fy + fh - y else h)
                    y += h
                }
                val left = min(fx + fw, fx + thickness)
                fw -= left - fx
                fx = left
            } else {
                var x = fx
                for (k in i until j) {
                    val w = weights[order[k]] * scale / thickness
                    out[order[k]] = StrataRect(x, fy, if (k + 1 == j) fx + fw - x else w, thickness)
                    x += w
                }
                val top = min(fy + fh, fy + thickness)
                fh -= top - fy
                fy = top
            }
            i = j
        }
        return out.toList()
    }

    fun treemapSize(file: InsightFile, sizeBy: StrataTreemapSize): Double = when (sizeBy) {
        StrataTreemapSize.Lines -> file.codeLines.toDouble()
        StrataTreemapSize.Complexity -> if (file.codeLines > 0) max(1, file.complexity).toDouble() else 0.0
        StrataTreemapSize.Functions -> if (file.codeLines > 0) max(1, file.functions.size).toDouble() else 0.0
    }

    fun treemap(
        snapshot: InsightSnapshot,
        graph: InsightModuleGraph,
        sizeBy: StrataTreemapSize,
        area: StrataRect,
        headerHeight: Double = 17.0,
        headerMinHeight: Double = 44.0,
        headerMinWidth: Double = 70.0,
        inset: Double = 2.0,
    ): StrataTreemap {
        val files = snapshot.files
        val moduleWeights = graph.nodes.map { node -> node.files.sumOf { treemapSize(files[it], sizeBy) } }
        val moduleRects = squarify(moduleWeights, area)
        val fileRects = Array(files.size) { StrataRect() }
        for ((m, node) in graph.nodes.withIndex()) {
            val box = moduleRects[m]
            if (box.width < inset || box.height < inset) continue
            val header = if (box.height > headerMinHeight && box.width > headerMinWidth) headerHeight else 0.0
            val inner = box.adjusted(inset, inset + header, -inset, -inset)
            val cells = squarify(node.files.map { treemapSize(files[it], sizeBy) }, inner)
            for ((k, cell) in cells.withIndex()) fileRects[node.files[k]] = cell
        }
        return StrataTreemap(moduleRects, fileRects.toList())
    }

    fun edgeWidth(weight: Int): Double = (1.0 + 1.25 * log2(1.0 + weight)).coerceIn(1.0, 7.0)
}
