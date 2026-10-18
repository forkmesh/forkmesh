package com.forkmesh.nativeapp.codeinsight

import kotlin.math.cosh
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sinh
import kotlin.math.sqrt
import kotlin.math.tanh

class EmberNode(
    val name: String,
    val path: String,
    val parent: Int,
    val depth: Int,
    val file: Int,
) {
    val children: MutableList<Int> = ArrayList()
    var x: Double = 0.0
    var y: Double = 0.0
    var r: Double = 0.0
    var codeLines: Long = 0
    var files: Int = 0
    var health: Double = 10.0
    var commits: Int = 0

    val isFolder: Boolean get() = file < 0
}

class EmberLayout(val nodes: List<EmberNode>, val nodeOfFile: IntArray, val maxDepth: Int) {
    val isEmpty: Boolean get() = nodes.isEmpty()

    fun isFolder(node: Int): Boolean = node >= 0 && node < nodes.size && nodes[node].file < 0

    fun contains(node: Int, x: Double, y: Double): Boolean {
        if (node < 0 || node >= nodes.size) return false
        val n = nodes[node]
        val dx = x - n.x
        val dy = y - n.y
        return dx * dx + dy * dy <= n.r * n.r
    }

    fun nodeAt(x: Double, y: Double): Int {
        if (nodes.isEmpty() || !contains(0, x, y)) return -1
        var current = 0
        while (true) {
            val next = nodes[current].children.firstOrNull { contains(it, x, y) } ?: return current
            current = next
        }
    }

    fun chain(node: Int): List<Int> {
        val out = ArrayList<Int>()
        var at = node
        while (at >= 0 && at < nodes.size) {
            out.add(at)
            at = nodes[at].parent
        }
        out.reverse()
        return out
    }

    fun isAncestor(ancestor: Int, node: Int): Boolean {
        var at = node
        while (at >= 0 && at < nodes.size) {
            if (at == ancestor) return true
            at = nodes[at].parent
        }
        return false
    }

    fun fileNode(file: Int): Int = if (file >= 0 && file < nodeOfFile.size) nodeOfFile[file] else -1

    companion object {
        val EMPTY = EmberLayout(emptyList(), IntArray(0), 0)
    }
}

data class EmberLayoutOptions(
    val minLines: Double = 12.0,
    val padding: Double = 2.5,
    val rimShare: Double = 0.035,
    val collapseChains: Boolean = true,
)

data class EmberView(val cx: Double = 0.0, val cy: Double = 0.0, val k: Double = 1.0)

object EmberMap {
    private class BuildNode(val name: String, val path: String, val file: Int = -1) {
        val children = ArrayList<Int>()
        val folders = HashMap<String, Int>()
    }

    fun buildLayout(snapshot: InsightSnapshot, options: EmberLayoutOptions = EmberLayoutOptions()): EmberLayout {
        val build = ArrayList<BuildNode>(snapshot.files.size * 2 + 1)
        build.add(BuildNode(snapshot.name.ifEmpty { "Repository" }, ""))
        snapshot.files.forEachIndexed { i, file ->
            val parts = file.path.split('/').filter { it.isNotEmpty() }
            if (parts.isEmpty()) return@forEachIndexed
            var current = 0
            var prefix = ""
            for (p in 0 until parts.size - 1) {
                prefix = if (prefix.isEmpty()) parts[p] else prefix + "/" + parts[p]
                val found = build[current].folders[parts[p]]
                if (found != null) {
                    current = found
                    continue
                }
                build.add(BuildNode(parts[p], prefix))
                val index = build.size - 1
                build[current].folders[parts[p]] = index
                build[current].children.add(index)
                current = index
            }
            build.add(BuildNode(parts.last(), file.path, i))
            build[current].children.add(build.size - 1)
        }

        val nodeOfFile = IntArray(snapshot.files.size) { -1 }
        val nodes = ArrayList<EmberNode>(build.size)
        var maxDepth = 0
        val stack = ArrayList<IntArray>()
        stack.add(intArrayOf(0, -1, 0))
        while (stack.isNotEmpty()) {
            val item = stack.removeAt(stack.size - 1)
            var source = item[0]
            val parent = item[1]
            val depth = item[2]
            var name = build[source].name
            var path = build[source].path
            val file = build[source].file
            if (options.collapseChains && parent >= 0 && file < 0) {
                while (build[source].children.size == 1 && build[build[source].children[0]].file < 0) {
                    source = build[source].children[0]
                    name += "/" + build[source].name
                    path = build[source].path
                }
            }
            nodes.add(EmberNode(name, path, parent, depth, file))
            val index = nodes.size - 1
            maxDepth = max(maxDepth, depth)
            if (parent >= 0) nodes[parent].children.add(index)
            if (file >= 0) nodeOfFile[file] = index
            val children = build[source].children
            for (c in children.indices.reversed()) stack.add(intArrayOf(children[c], index, depth + 1))
        }

        val order = Comparator<Int> { a, b ->
            val na = nodes[a]
            val nb = nodes[b]
            if (na.r != nb.r) nb.r.compareTo(na.r) else na.name.compareTo(nb.name)
        }
        for (i in nodes.indices.reversed()) {
            val node = nodes[i]
            if (node.file >= 0) {
                val file = snapshot.files[node.file]
                node.codeLines = file.codeLines.toLong()
                node.files = 1
                node.health = file.health
                node.commits = file.commits
                node.r = sqrt(max(file.codeLines.toDouble(), options.minLines))
                continue
            }
            var weight = 0.0
            var lines = 0L
            for (child in node.children) {
                val c = nodes[child]
                node.codeLines += c.codeLines
                node.files += c.files
                node.commits += c.commits
                val l = max(1L, c.codeLines)
                lines += l
                weight += c.health * l
            }
            node.health = if (lines > 0) weight / lines else 10.0
            node.children.sortWith(order)
            val circles = node.children.map { EmberCircle(0.0, 0.0, nodes[it].r + options.padding) }
            val enclosing = EmberPack.packSiblings(circles)
            node.children.forEachIndexed { c, child ->
                nodes[child].x = circles[c].x
                nodes[child].y = circles[c].y
            }
            node.r = enclosing + options.padding + enclosing * options.rimShare
        }
        for (i in 1 until nodes.size) {
            val node = nodes[i]
            val parent = nodes[node.parent]
            node.x += parent.x
            node.y += parent.y
        }
        return EmberLayout(nodes, nodeOfFile, maxDepth)
    }

    fun fitTransform(node: EmberNode, width: Double, height: Double, margin: Double = 18.0): EmberView {
        val half = max(10.0, min(width, height) / 2 - margin)
        return EmberView(node.x, node.y, if (node.r > 0) half / node.r else 1.0)
    }

    fun interpolateView(from: EmberView, to: EmberView, t: Double, width: Double, height: Double): EmberView {
        val size = max(1.0, min(width, height))
        val rho = sqrt(2.0)
        val rho2 = 2.0
        val rho4 = 4.0
        val w0 = size / from.k
        val w1 = size / to.k
        val dx = to.cx - from.cx
        val dy = to.cy - from.cy
        val d2 = dx * dx + dy * dy
        if (d2 < 1e-12) {
            val s = ln(w1 / w0) / rho
            return EmberView(from.cx + t * dx, from.cy + t * dy, size / (w0 * exp(rho * t * s)))
        }
        val d1 = sqrt(d2)
        val b0 = (w1 * w1 - w0 * w0 + rho4 * d2) / (2 * w0 * rho2 * d1)
        val b1 = (w1 * w1 - w0 * w0 - rho4 * d2) / (2 * w1 * rho2 * d1)
        val r0 = ln(sqrt(b0 * b0 + 1) - b0)
        val r1 = ln(sqrt(b1 * b1 + 1) - b1)
        val s = t * (r1 - r0) / rho
        val coshr0 = cosh(r0)
        val u = w0 / (rho2 * d1) * (coshr0 * tanh(rho * s + r0) - sinh(r0))
        var out = EmberView(from.cx + u * dx, from.cy + u * dy, size / (w0 * coshr0 / cosh(rho * s + r0)))
        if (!out.cx.isFinite() || !out.cy.isFinite() || !out.k.isFinite() || out.k <= 0) {
            out = EmberView(from.cx + t * dx, from.cy + t * dy, exp(ln(from.k) + t * (ln(to.k) - ln(from.k))))
        }
        if (t >= 1) out = to
        return out
    }

    fun easeInOut(value: Double): Double {
        val t = value.coerceIn(0.0, 1.0)
        return if (t < 0.5) 4 * t * t * t else 1 - (-2 * t + 2).pow(3) / 2
    }

    fun readableFocus(layout: EmberLayout, node: Int, width: Double, height: Double, minRadius: Double = 48.0, margin: Double = 18.0): Int {
        if (node < 0 || node >= layout.nodes.size) return if (layout.nodes.isEmpty()) -1 else 0
        var candidate = if (layout.isFolder(node)) node else layout.nodes[node].parent
        if (candidate < 0) return 0
        var best = candidate
        while (candidate >= 0) {
            val fit = fitTransform(layout.nodes[candidate], width, height, margin)
            if (layout.nodes[node].r * fit.k < minRadius) break
            best = candidate
            candidate = layout.nodes[candidate].parent
        }
        return best
    }

    fun toScreenX(view: EmberView, x: Double, width: Double): Double = (x - view.cx) * view.k + width / 2

    fun toScreenY(view: EmberView, y: Double, height: Double): Double = (y - view.cy) * view.k + height / 2

    fun toWorldX(view: EmberView, x: Double, width: Double): Double = (x - width / 2) / view.k + view.cx

    fun toWorldY(view: EmberView, y: Double, height: Double): Double = (y - height / 2) / view.k + view.cy

    fun minScale(layout: EmberLayout, width: Double, height: Double, margin: Double = 18.0): Double =
        if (layout.isEmpty) 1e-6 else fitTransform(layout.nodes[0], width, height, margin).k * 0.6

    fun maxScale(layout: EmberLayout, width: Double, height: Double, density: Double = 1.0, margin: Double = 18.0): Double =
        max(minScale(layout, width, height, margin) * 2, 90.0 * density)

    fun zoomAround(view: EmberView, factor: Double, px: Double, py: Double, width: Double, height: Double, minK: Double, maxK: Double): EmberView {
        val ax = toWorldX(view, px, width)
        val ay = toWorldY(view, py, height)
        val k = (view.k * factor).coerceIn(minK, maxK)
        return EmberView(ax - (px - width / 2) / k, ay - (py - height / 2) / k, k)
    }

    fun focusFromView(layout: EmberLayout, view: EmberView, width: Double, height: Double): Int {
        if (layout.isEmpty) return 0
        val need = 0.42 * min(width, height)
        val chain = layout.chain(layout.nodeAt(toWorldX(view, width / 2, width), toWorldY(view, height / 2, height)))
        var focus = 0
        for (node in chain) {
            if (layout.isFolder(node) && layout.nodes[node].r * view.k >= need) focus = node
        }
        return focus
    }

    sealed interface Tap {
        data object None : Tap

        data class SelectFile(val file: Int) : Tap

        data class Focus(val node: Int) : Tap

        data object ZoomOut : Tap

        data object ClearSelection : Tap
    }

    fun tapAt(
        layout: EmberLayout,
        view: EmberView,
        focus: Int,
        selected: Int,
        x: Double,
        y: Double,
        width: Double,
        height: Double,
        fileRadius: Double = 2.5,
        folderRadius: Double = 14.0,
    ): Tap {
        if (layout.isEmpty) return Tap.None
        val deepest = layout.nodeAt(toWorldX(view, x, width), toWorldY(view, y, height))
        if (deepest < 0) {
            return when {
                focus != 0 -> Tap.ZoomOut
                selected >= 0 -> Tap.ClearSelection
                else -> Tap.None
            }
        }
        if (!layout.isFolder(deepest) && layout.nodes[deepest].r * view.k >= fileRadius) return Tap.SelectFile(layout.nodes[deepest].file)
        var target = -1
        for (node in layout.chain(deepest)) {
            if (node != focus && layout.isFolder(node) && layout.isAncestor(focus, node) && layout.nodes[node].r * view.k >= folderRadius) target = node
        }
        if (target >= 0) return Tap.Focus(target)
        if (!layout.isAncestor(focus, deepest) && focus != 0) return Tap.ZoomOut
        if (selected >= 0) return Tap.ClearSelection
        return Tap.None
    }

    fun crumbs(layout: EmberLayout, focus: Int, selectedNode: Int): List<EmberCrumb> {
        if (layout.isEmpty) return emptyList()
        val chain = layout.chain(focus)
        val parts = ArrayList<EmberCrumb>()
        if (chain.size > 6) {
            parts.add(EmberCrumb(layout.nodes[chain.first()].name, chain.first()))
            parts.add(EmberCrumb("…", -1))
            for (i in chain.size - 4 until chain.size) parts.add(EmberCrumb(layout.nodes[chain[i]].name, chain[i]))
        } else {
            for (node in chain) parts.add(EmberCrumb(layout.nodes[node].name, node))
        }
        if (selectedNode >= 0 && selectedNode < layout.nodes.size) {
            val fileChain = layout.chain(selectedNode)
            val at = if (chain.isEmpty()) -1 else fileChain.indexOf(chain.last())
            if (at >= 0) {
                for (i in at + 1 until fileChain.size - 1) parts.add(EmberCrumb(layout.nodes[fileChain[i]].name, fileChain[i]))
            } else {
                parts.add(EmberCrumb("…", -1))
            }
            parts.add(EmberCrumb(layout.nodes[selectedNode].name, selectedNode, current = true))
        }
        return parts
    }
}

data class EmberCrumb(val name: String, val node: Int, val current: Boolean = false)
