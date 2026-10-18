package com.forkmesh.nativeapp.codeinsight

import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tan

enum class SynapseColourBy(val label: String) {
    Module("Module"),
    Language("Language"),
    Health("Health"),
    Hotspot("Hotspot"),
}

enum class SynapseShowMode(val label: String) {
    Everything("Everything"),
    Cycles("Circular only"),
    Orphans("Orphans"),
    Neighbourhood("Neighbourhood"),
}

object SynapseColours {
    const val BACKGROUND: Int = 0xFF070B16.toInt()
    const val CYCLE: Int = 0xFFFF5450.toInt()
    const val IMPORT: Int = 0xFF3CC8FF.toInt()
    const val IMPORTER: Int = 0xFFBC8CFF.toInt()
    const val SELECTED: Int = 0xFFECF6FF.toInt()
    const val TEXT: Int = 0xFFE6EDF3.toInt()
    const val MUTED: Int = 0xFF8B949E.toInt()
    const val WHITE: Int = 0xFFFFFFFF.toInt()

    private val palette = intArrayOf(
        0xFF6CB6FF.toInt(), 0xFF56D4DD.toInt(), 0xFFFFA657.toInt(),
        0xFFD2A8FF.toInt(), 0xFFE3C06D.toInt(), 0xFF79E2C0.toInt(),
        0xFFF2A7E0.toInt(), 0xFFA5B4FC.toInt(), 0xFF9EE7FF.toInt(),
        0xFFFFC680.toInt(), 0xFFB49CFF.toInt(), 0xFF8FD3B6.toInt(),
    )

    private val languages = mapOf(
        "C++" to 0xFFF34B7D.toInt(), "C" to 0xFF8FA3B8.toInt(), "TypeScript" to 0xFF4A9BF0.toInt(),
        "JavaScript" to 0xFFF1E05A.toInt(), "Python" to 0xFF5B8FD6.toInt(), "Go" to 0xFF00C0E8.toInt(),
        "Rust" to 0xFFDEA584.toInt(), "Java" to 0xFFD08A3C.toInt(), "Kotlin" to 0xFFA97BFF.toInt(),
        "Swift" to 0xFFF06A4A.toInt(), "C#" to 0xFF9B6FD6.toInt(), "Ruby" to 0xFFE04A5F.toInt(),
        "PHP" to 0xFF8C92D8.toInt(), "Dart" to 0xFF3DC5C5.toInt(), "Objective-C" to 0xFF649CF0.toInt(),
        "Shell" to 0xFF89E051.toInt(), "QML" to 0xFF44D08E.toInt(), "Vue" to 0xFF41D89A.toInt(),
        "Svelte" to 0xFFFF6E40.toInt(), "CSS" to 0xFF9A7FE0.toInt(), "SCSS" to 0xFFD66FA8.toInt(),
        "Lua" to 0xFF607CF0.toInt(), "Scala" to 0xFFE05048.toInt(),
    )

    fun lobe(index: Int): Int = palette[((index % palette.size) + palette.size) % palette.size]

    fun language(name: String): Int = languages[name] ?: lobe((Synapse.stableHashUnsigned(name) % 12L).toInt())

    fun health(health: Double): Int = when {
        health >= 8 -> 0xFF3FB98F.toInt()
        health >= 4 -> 0xFFE3B341.toInt()
        else -> 0xFFE5534B.toInt()
    }

    fun hotspot(hotspot: Double): Int {
        val t = hotspot.coerceIn(0.0, 1.0).toFloat()
        val cool = 0xFF3A4D72.toInt()
        val warm = 0xFFE3A23C.toInt()
        val hot = 0xFFFF6F3C.toInt()
        return if (t < 0.5f) mix(cool, warm, t * 2.0f) else mix(warm, hot, (t - 0.5f) * 2.0f)
    }

    fun alpha(colour: Int): Float = ((colour ushr 24) and 0xff) / 255f

    private fun channel(colour: Int, shift: Int): Float = ((colour ushr shift) and 0xff) / 255f

    fun argb(a: Float, r: Float, g: Float, b: Float): Int {
        fun byte(v: Float): Int = (v.coerceIn(0f, 1f) * 255f + 0.5f).toInt()
        return (byte(a) shl 24) or (byte(r) shl 16) or (byte(g) shl 8) or byte(b)
    }

    fun mix(a: Int, b: Int, t: Float): Int {
        val u = 1.0f - t
        return argb(
            channel(a, 24) * u + channel(b, 24) * t,
            channel(a, 16) * u + channel(b, 16) * t,
            channel(a, 8) * u + channel(b, 8) * t,
            channel(a, 0) * u + channel(b, 0) * t,
        )
    }

    fun withAlpha(colour: Int, alpha: Float): Int = (colour and 0x00ffffff) or ((alpha.coerceIn(0f, 1f) * 255f + 0.5f).toInt() shl 24)

    fun lighter(colour: Int, factor: Int): Int {
        val r = channel(colour, 16)
        val g = channel(colour, 8)
        val b = channel(colour, 0)
        val maxC = max(r, max(g, b))
        val minC = min(r, min(g, b))
        if (maxC <= 0f) {
            val v = (factor / 100f - 1f).coerceIn(0f, 1f)
            return argb(channel(colour, 24), v, v, v)
        }
        var s = (maxC - minC) / maxC
        var v = maxC * factor / 100f
        if (v > 1f) {
            s = max(0f, s - (v - 1f))
            v = 1f
        }
        val hue = when {
            maxC == minC -> 0f
            maxC == r -> ((g - b) / (maxC - minC) + 6f) % 6f
            maxC == g -> (b - r) / (maxC - minC) + 2f
            else -> (r - g) / (maxC - minC) + 4f
        }
        val c = v * s
        val x = c * (1f - abs(hue % 2f - 1f))
        val m = v - c
        val (rr, gg, bb) = when (hue.toInt().coerceIn(0, 5)) {
            0 -> Triple(c, x, 0f)
            1 -> Triple(x, c, 0f)
            2 -> Triple(0f, c, x)
            3 -> Triple(0f, x, c)
            4 -> Triple(x, 0f, c)
            else -> Triple(c, 0f, x)
        }
        return argb(channel(colour, 24), rr + m, gg + m, bb + m)
    }
}

data class SynapseSceneOptions(
    val colourBy: SynapseColourBy = SynapseColourBy.Module,
    val showMode: SynapseShowMode = SynapseShowMode.Everything,
    val depth: Int = 1,
    val selected: Int = -1,
    val isolatedCycle: Int = -1,
    val query: String = "",
    val pulses: Boolean = true,
    val edgeCap: Int = 5000,
    val dimCap: Int = 1200,
)

class SynapseStyle(
    val graph: SynapseGraph,
    val options: SynapseSceneOptions,
    val nodeState: ByteArray,
    val nodeColour: IntArray,
    val baseColour: IntArray,
    val edgeState: ByteArray,
    val drawEdges: IntArray,
    val hubs: IntArray,
    val neighbours: SynapseNeighbourhood,
    val visibleNodes: Int,
    val cycleEdges: Int,
    val pulseEdges: Int,
    val cycleNodesVisible: Boolean,
) {
    val selectedNode: Int get() = graph.nodeFor(options.selected)

    fun visibleNodeIndexes(): IntArray = graph.nodes.indices.filter { nodeState[it] != SynapseScene.HIDDEN }.toIntArray()

    fun nodeRadius(node: Int): Float {
        val base = min(graph.spacing * 0.15f, 3.4f) * graph.nodes[node].size
        return when (nodeState[node]) {
            SynapseScene.SELECTED -> base * 1.55f
            SynapseScene.IMPORT, SynapseScene.IMPORTER, SynapseScene.MATCH -> base * 1.15f
            SynapseScene.DIM -> base * 0.85f
            else -> base
        }
    }

    fun edgeWidth(edge: Int): Float = when (edgeState[edge]) {
        SynapseScene.EDGE_IMPORT, SynapseScene.EDGE_IMPORTER -> 1.5f
        SynapseScene.EDGE_CYCLE -> if (cycleEdges > 300) 0.95f else 1.35f
        SynapseScene.EDGE_DIM -> 0.6f
        else -> 0.85f
    }

    fun edgePulses(edge: Int): Boolean {
        if (!options.pulses) return false
        val state = edgeState[edge]
        if (state == SynapseScene.EDGE_IMPORT || state == SynapseScene.EDGE_IMPORTER || state == SynapseScene.EDGE_CYCLE) return true
        return state == SynapseScene.EDGE_NORMAL && graph.edges.size <= 1500
    }

    fun highlighted(edge: Int): Boolean {
        val state = edgeState[edge]
        return state == SynapseScene.EDGE_IMPORT || state == SynapseScene.EDGE_IMPORTER || state == SynapseScene.EDGE_CYCLE
    }

    fun edgeColour(edge: Int, t: Float): Int {
        val e = graph.edges[edge]
        when (edgeState[edge]) {
            SynapseScene.EDGE_IMPORT -> return SynapseColours.withAlpha(SynapseColours.mix(SynapseColours.IMPORT, 0xFF96E6FF.toInt(), t * 0.5f), 0.85f)
            SynapseScene.EDGE_IMPORTER -> return SynapseColours.withAlpha(SynapseColours.mix(0xFFDCBEFF.toInt(), SynapseColours.IMPORTER, t), 0.85f)
            SynapseScene.EDGE_CYCLE -> return SynapseColours.withAlpha(SynapseColours.CYCLE, if (cycleEdges > 300) 0.3f else 0.8f)
            SynapseScene.EDGE_DIM -> return SynapseColours.withAlpha(SynapseColours.mix(nodeColour[e.from], nodeColour[e.to], t), 0.05f)
        }
        val drawn = max(1, drawEdges.size)
        val alpha = (0.5f / sqrt(max(1.0f, drawn / 120.0f))).coerceIn(0.08f, 0.42f)
        val a = baseColour[e.from]
        val b = baseColour[e.to]
        val tone = if (e.cycle) SynapseColours.mix(SynapseColours.mix(a, b, t), SynapseColours.CYCLE, 0.45f) else SynapseColours.mix(a, b, t)
        return SynapseColours.withAlpha(SynapseColours.mix(tone, SynapseColours.WHITE, 0.18f), alpha)
    }

    fun animating(autoRotate: Boolean): Boolean = autoRotate || (options.pulses && (pulseEdges > 0 || cycleNodesVisible))
}

data class SynapseCamera(
    val yaw: Float = 1.05f,
    val pitch: Float = 0.32f,
    val distance: Float = 420.0f,
    val targetX: Float = 0f,
    val targetY: Float = 0f,
    val targetZ: Float = 0f,
) {
    fun clamped(): SynapseCamera = copy(pitch = pitch.coerceIn(-1.45f, 1.45f), distance = distance.coerceIn(12.0f, 2400.0f))
}

class SynapseProjection {
    var size = 0
        private set
    var x = FloatArray(0)
        private set
    var y = FloatArray(0)
        private set
    var depth = FloatArray(0)
        private set
    var radius = FloatArray(0)
        private set
    var fog = FloatArray(0)
        private set
    var onScreen = BooleanArray(0)
        private set
    var width = 1f
        private set
    var height = 1f
        private set
    var focal = 1f
        private set
    var fogNear = 0f
        private set
    var fogFar = 1f
        private set
    var nearPlane = 1f
        private set
    var px = 0f
        private set
    var py = 0f
        private set
    var pDepth = 0f
        private set

    private var eyeX = 0f
    private var eyeY = 0f
    private var eyeZ = 0f
    private var rightX = 0f
    private var rightY = 0f
    private var rightZ = 0f
    private var upX = 0f
    private var upY = 0f
    private var upZ = 0f
    private var forwardX = 0f
    private var forwardY = 0f
    private var forwardZ = 0f

    private fun ensure(n: Int) {
        if (x.size >= n) return
        val capacity = max(n, x.size * 2)
        x = FloatArray(capacity)
        y = FloatArray(capacity)
        depth = FloatArray(capacity)
        radius = FloatArray(capacity)
        fog = FloatArray(capacity)
        onScreen = BooleanArray(capacity)
    }

    fun setCamera(camera: SynapseCamera, width: Float, height: Float, boundingRadius: Float) {
        this.width = max(1f, width)
        this.height = max(1f, height)
        val cp = cos(camera.pitch)
        val bx = cp * sin(camera.yaw)
        val by = sin(camera.pitch)
        val bz = cp * cos(camera.yaw)
        eyeX = camera.targetX + bx * camera.distance
        eyeY = camera.targetY + by * camera.distance
        eyeZ = camera.targetZ + bz * camera.distance
        forwardX = -bx
        forwardY = -by
        forwardZ = -bz
        var rx = -forwardZ
        var rz = forwardX
        val rl = hypot(rx, rz)
        if (rl > 1e-6f) {
            rx /= rl
            rz /= rl
        } else {
            rx = 1f
            rz = 0f
        }
        rightX = rx
        rightY = 0f
        rightZ = rz
        upX = rightY * forwardZ - rightZ * forwardY
        upY = rightZ * forwardX - rightX * forwardZ
        upZ = rightX * forwardY - rightY * forwardX
        focal = this.height * 0.5f / tan(Math.toRadians(SynapseScene.FOV_DEGREES * 0.5).toFloat())
        nearPlane = max(1.0f, camera.distance * 0.05f)
        val centreDepth = (0f - eyeX) * forwardX + (0f - eyeY) * forwardY + (0f - eyeZ) * forwardZ
        fogNear = centreDepth - boundingRadius * 0.55f
        fogFar = centreDepth + boundingRadius * 1.1f
    }

    fun project(wx: Float, wy: Float, wz: Float): Boolean {
        val dx = wx - eyeX
        val dy = wy - eyeY
        val dz = wz - eyeZ
        val d = dx * forwardX + dy * forwardY + dz * forwardZ
        pDepth = d
        if (d <= 0.5f) return false
        val scale = focal / d
        px = width * 0.5f + (dx * rightX + dy * rightY + dz * rightZ) * scale
        py = height * 0.5f - (dx * upX + dy * upY + dz * upZ) * scale
        return true
    }

    fun fogAt(d: Float, amount: Float = 0.72f): Float =
        1.0f - amount * ((d - fogNear) / max(1.0f, fogFar - fogNear)).coerceIn(0.0f, 1.0f)

    fun update(style: SynapseStyle, positions: FloatArray, camera: SynapseCamera, width: Float, height: Float) {
        val graph = style.graph
        setCamera(camera, width, height, graph.shape.boundingRadius)
        val n = graph.nodes.size
        ensure(n)
        size = n
        for (i in 0 until n) {
            onScreen[i] = false
            depth[i] = 0f
            if (style.nodeState[i] == SynapseScene.HIDDEN || i * 3 + 2 >= positions.size) continue
            val visible = project(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2])
            if (!visible || pDepth < nearPlane) continue
            x[i] = px
            y[i] = py
            depth[i] = pDepth
            radius[i] = style.nodeRadius(i) * focal / pDepth
            fog[i] = fogAt(pDepth)
            val margin = radius[i] * 3.0f + 2.0f
            onScreen[i] = px > -margin && px < this.width + margin && py > -margin && py < this.height + margin
        }
    }
}

object SynapseScene {
    const val NORMAL: Byte = 0
    const val DIM: Byte = 1
    const val SELECTED: Byte = 2
    const val IMPORT: Byte = 3
    const val IMPORTER: Byte = 4
    const val MATCH: Byte = 5
    const val HIDDEN: Byte = 6

    const val EDGE_NORMAL: Byte = 0
    const val EDGE_DIM: Byte = 1
    const val EDGE_IMPORT: Byte = 2
    const val EDGE_IMPORTER: Byte = 3
    const val EDGE_CYCLE: Byte = 4
    const val EDGE_HIDDEN: Byte = 5

    const val FOV_DEGREES = 38.0
    const val BOW = 0.26f

    fun baseColour(graph: SynapseGraph, node: Int, colourBy: SynapseColourBy): Int {
        val n = graph.nodes[node]
        val file = graph.snapshot.files[n.file]
        return when (colourBy) {
            SynapseColourBy.Module -> SynapseColours.lobe(n.lobe)
            SynapseColourBy.Language -> SynapseColours.language(file.language)
            SynapseColourBy.Health -> SynapseColours.health(file.health)
            SynapseColourBy.Hotspot -> SynapseColours.hotspot(file.hotspot)
        }
    }

    fun restyle(graph: SynapseGraph, options: SynapseSceneOptions): SynapseStyle {
        val snapshot = graph.snapshot
        val n = graph.nodes.size
        val visible = BooleanArray(n) { true }
        val isolated = options.isolatedCycle >= 0 && options.isolatedCycle < graph.cycles.size
        if (isolated) {
            visible.fill(false)
            for (file in graph.cycles[options.isolatedCycle].files) {
                val node = graph.nodeFor(file)
                if (node >= 0) visible[node] = true
            }
        } else if (options.showMode == SynapseShowMode.Cycles) {
            for (i in 0 until n) visible[i] = graph.nodes[i].cycle
        } else if (options.showMode == SynapseShowMode.Orphans) {
            for (i in 0 until n) visible[i] = graph.nodes[i].fanIn == 0 && graph.nodes[i].fanOut == 0
        }
        val selected = if (options.selected >= 0 && options.selected < snapshot.files.size) options.selected else -1
        val depth = options.depth.coerceIn(1, 3)
        val neighbours = if (selected >= 0) Synapse.neighbourhood(snapshot, selected, depth, SynapseDirection.Both) else SynapseNeighbourhood()
        if (options.showMode == SynapseShowMode.Neighbourhood && selected >= 0 && !isolated) {
            for (i in 0 until n) {
                val file = graph.nodes[i].file
                visible[i] = file == selected || neighbours.contains(file)
            }
        }
        val query = options.query.trim()
        val searching = query.isNotEmpty() && selected < 0
        val nodeState = ByteArray(n)
        val nodeColour = IntArray(n)
        val baseColour = IntArray(n)
        var visibleNodes = 0
        var cycleNodesVisible = false
        for (i in 0 until n) {
            val node = graph.nodes[i]
            val state = when {
                !visible[i] -> HIDDEN
                selected >= 0 -> when {
                    node.file == selected -> SELECTED
                    neighbours.imports.containsKey(node.file) -> IMPORT
                    neighbours.importers.containsKey(node.file) -> IMPORTER
                    else -> DIM
                }
                searching -> if (snapshot.files[node.file].path.contains(query, ignoreCase = true)) MATCH else DIM
                else -> NORMAL
            }
            nodeState[i] = state
            if (state != HIDDEN) visibleNodes++
            if (state == NORMAL && node.cycle) cycleNodesVisible = true
            val base = baseColour(graph, i, options.colourBy)
            baseColour[i] = base
            nodeColour[i] = when (state) {
                SELECTED -> SynapseColours.SELECTED
                IMPORT -> SynapseColours.IMPORT
                IMPORTER -> SynapseColours.IMPORTER
                MATCH -> if (node.cycle) SynapseColours.CYCLE else SynapseColours.lighter(base, 125)
                DIM -> SynapseColours.mix(if (node.cycle) SynapseColours.CYCLE else base, SynapseColours.BACKGROUND, 0.62f)
                else -> if (node.cycle) SynapseColours.CYCLE else base
            }
        }
        val edgeState = ByteArray(graph.edges.size)
        val highlighted = ArrayList<Int>()
        val normal = ArrayList<Int>()
        val dim = ArrayList<Int>()
        for ((e, edge) in graph.edges.withIndex()) {
            val a = nodeState[edge.from]
            val b = nodeState[edge.to]
            val state = when {
                a == HIDDEN || b == HIDDEN -> EDGE_HIDDEN
                selected >= 0 -> {
                    val fa = graph.nodes[edge.from].file
                    val fb = graph.nodes[edge.to].file
                    val ia = if (fa == selected) 0 else neighbours.imports[fa] ?: -1
                    val ib = neighbours.imports[fb] ?: -1
                    val ra = neighbours.importers[fa] ?: -1
                    val rb = if (fb == selected) 0 else neighbours.importers[fb] ?: -1
                    when {
                        ia >= 0 && ib == ia + 1 -> EDGE_IMPORT
                        rb >= 0 && ra == rb + 1 -> EDGE_IMPORTER
                        else -> EDGE_DIM
                    }
                }
                searching -> if (a == MATCH && b == MATCH) EDGE_NORMAL else EDGE_DIM
                edge.cycle && (isolated || smallCycle(graph, edge)) -> EDGE_CYCLE
                else -> EDGE_NORMAL
            }
            edgeState[e] = state
            when (state) {
                EDGE_IMPORT, EDGE_IMPORTER, EDGE_CYCLE -> highlighted.add(e)
                EDGE_NORMAL -> normal.add(e)
                EDGE_DIM -> dim.add(e)
            }
        }
        val bySeed = Comparator<Int> { x, y ->
            val sx = graph.edges[x].seed.toLong() and 0xffffffffL
            val sy = graph.edges[y].seed.toLong() and 0xffffffffL
            if (sx != sy) sx.compareTo(sy) else x.compareTo(y)
        }
        val cap = options.edgeCap
        var keptHighlighted: List<Int> = highlighted
        if (highlighted.size > cap) keptHighlighted = highlighted.sortedWith(bySeed).subList(0, cap)
        var room = cap - keptHighlighted.size
        var keptNormal: List<Int> = normal
        if (normal.size > room) keptNormal = normal.sortedWith(bySeed).subList(0, max(0, room))
        room -= keptNormal.size
        var keptDim: List<Int> = dim
        val dimRoom = min(room, options.dimCap)
        if (dim.size > dimRoom) keptDim = dim.sortedWith(bySeed).subList(0, max(0, dimRoom))
        val drawEdges = IntArray(keptDim.size + keptNormal.size + keptHighlighted.size)
        var at = 0
        for (e in keptDim) drawEdges[at++] = e
        for (e in keptNormal) drawEdges[at++] = e
        for (e in keptHighlighted) drawEdges[at++] = e
        var cycleEdges = 0
        for (e in keptHighlighted) if (edgeState[e] == EDGE_CYCLE) cycleEdges++
        val hubs = (0 until n)
            .filter { nodeState[it] != HIDDEN && nodeState[it] != DIM && graph.nodes[it].fanIn > 0 }
            .sortedWith(compareByDescending<Int> { graph.nodes[it].fanIn }.thenBy { it })
            .take(10)
            .toIntArray()
        val style = SynapseStyle(
            graph = graph,
            options = options.copy(selected = selected, depth = depth),
            nodeState = nodeState,
            nodeColour = nodeColour,
            baseColour = baseColour,
            edgeState = edgeState,
            drawEdges = drawEdges,
            hubs = hubs,
            neighbours = neighbours,
            visibleNodes = visibleNodes,
            cycleEdges = cycleEdges,
            pulseEdges = 0,
            cycleNodesVisible = cycleNodesVisible,
        )
        var pulseEdges = 0
        for (e in drawEdges) if (style.edgePulses(e)) pulseEdges++
        return SynapseStyle(
            graph, style.options, nodeState, nodeColour, baseColour, edgeState, drawEdges, hubs, neighbours,
            visibleNodes, cycleEdges, pulseEdges, cycleNodesVisible,
        )
    }

    private fun smallCycle(graph: SynapseGraph, edge: SynapseEdge): Boolean {
        val cycle = graph.snapshot.files[graph.nodes[edge.from].file].cycle
        return cycle >= 0 && cycle < graph.snapshot.cycles.size && graph.snapshot.cycles[cycle].size <= 40
    }

    fun eyeBack(camera: SynapseCamera): FloatArray {
        val cp = cos(camera.pitch)
        return floatArrayOf(cp * sin(camera.yaw), sin(camera.pitch), cp * cos(camera.yaw))
    }

    fun fittedCamera(
        style: SynapseStyle,
        positions: FloatArray,
        camera: SynapseCamera,
        nodes: IntArray?,
        aspect: Float,
        autoRotate: Boolean,
        settled: Boolean,
    ): SynapseCamera {
        val graph = style.graph
        val points = ArrayList<FloatArray>()
        val ready = positions.size == graph.nodes.size * 3
        if (ready) {
            if (nodes == null || nodes.isEmpty()) {
                for (i in graph.nodes.indices) {
                    if (style.nodeState[i] != HIDDEN) points.add(floatArrayOf(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]))
                }
            } else {
                for (node in nodes) if (node >= 0 && node < graph.nodes.size) points.add(floatArrayOf(positions[node * 3], positions[node * 3 + 1], positions[node * 3 + 2]))
            }
        }
        val whole = (nodes == null || nodes.isEmpty()) && (points.size == graph.nodes.size || !settled)
        if (whole || points.isEmpty()) {
            val shape = graph.shape
            points.clear()
            for (sx in intArrayOf(-1, 1)) for (sz in intArrayOf(-1, 1)) {
                points.add(floatArrayOf(sx * shape.halfWidth * 0.8f, shape.halfHeightTop * 0.8f, sz * shape.halfLength * 0.8f))
                points.add(floatArrayOf(sx * shape.halfWidth * 0.8f, -shape.halfHeightBottom * 0.8f, sz * shape.halfLength * 0.8f))
            }
            points.add(floatArrayOf(0f, 0f, shape.halfLength))
            points.add(floatArrayOf(0f, 0f, -shape.halfLength))
            points.add(floatArrayOf(shape.halfWidth, 0f, 0f))
            points.add(floatArrayOf(-shape.halfWidth, 0f, 0f))
            points.add(floatArrayOf(0f, shape.halfHeightTop, 0f))
            points.add(floatArrayOf(0f, -shape.halfHeightBottom, 0f))
        }
        val lo = points[0].copyOf()
        val hi = points[0].copyOf()
        for (p in points) for (c in 0 until 3) {
            lo[c] = min(lo[c], p[c])
            hi[c] = max(hi[c], p[c])
        }
        val tx = (lo[0] + hi[0]) * 0.5f
        val ty = (lo[1] + hi[1]) * 0.5f
        val tz = (lo[2] + hi[2]) * 0.5f
        val back = eyeBack(camera)
        val fx = -back[0]
        val fy = -back[1]
        val fz = -back[2]
        var rx = -fz
        var rz = fx
        val rl = hypot(rx, rz).takeIf { it > 1e-6f } ?: 1f
        rx /= rl
        rz /= rl
        val ux = 0f * fz - rz * fy
        val uy = rz * fx - rx * fz
        val uz = rx * fy - 0f * fx
        val tanV = tan(Math.toRadians(FOV_DEGREES * 0.5).toFloat()) * 0.86f
        val tanH = tanV * max(0.4f, aspect)
        var distance = 0f
        for (p in points) {
            val qx = p[0] - tx
            val qy = p[1] - ty
            val qz = p[2] - tz
            val x = if (autoRotate) hypot(qx, qz) else abs(qx * rx + qz * rz)
            val y = abs(qx * ux + qy * uy + qz * uz)
            val z = qx * back[0] + qy * back[1] + qz * back[2]
            distance = max(distance, max(z + x / tanH, z + y / tanV))
        }
        val minimum = graph.spacing * 4.0f
        return camera.copy(targetX = tx, targetY = ty, targetZ = tz, distance = distance.coerceIn(max(30.0f, minimum), 2400.0f))
    }

    fun fileCamera(style: SynapseStyle, positions: FloatArray, camera: SynapseCamera, file: Int, aspect: Float, autoRotate: Boolean): SynapseCamera? {
        val graph = style.graph
        val node = graph.nodeFor(file)
        if (node < 0 || node * 3 + 2 >= positions.size) return null
        val nodes = ArrayList<Int>()
        nodes.add(node)
        for (e in graph.outEdges[node]) nodes.add(graph.edges[e].to)
        for (e in graph.inEdges[node]) nodes.add(graph.edges[e].from)
        val fitted = fittedCamera(style, positions, camera, nodes.toIntArray(), aspect, autoRotate, true)
        val distance = (fitted.distance * 1.1f).coerceIn(min(graph.spacing * 6.0f, camera.distance), camera.distance)
        return fitted.copy(
            targetX = positions[node * 3] * 0.7f + fitted.targetX * 0.3f,
            targetY = positions[node * 3 + 1] * 0.7f + fitted.targetY * 0.3f,
            targetZ = positions[node * 3 + 2] * 0.7f + fitted.targetZ * 0.3f,
            distance = distance,
        )
    }

    fun cycleCamera(style: SynapseStyle, positions: FloatArray, camera: SynapseCamera, cycle: Int, aspect: Float, autoRotate: Boolean): SynapseCamera? {
        val graph = style.graph
        if (cycle < 0 || cycle >= graph.cycles.size) return null
        val nodes = graph.cycles[cycle].files.map { graph.nodeFor(it) }.toIntArray()
        val fitted = fittedCamera(style, positions, camera, nodes, aspect, autoRotate, true)
        return fitted.copy(distance = max(fitted.distance * 1.15f, graph.spacing * 6.0f))
    }

    fun easeInOut(t: Float): Float = if (t < 0.5f) 4.0f * t * t * t else 1.0f - (-2.0f * t + 2.0f).let { it * it * it } / 2.0f

    fun interpolate(from: SynapseCamera, to: SynapseCamera, t: Float): SynapseCamera {
        val e = easeInOut(t.coerceIn(0f, 1f))
        return from.copy(
            targetX = from.targetX + (to.targetX - from.targetX) * e,
            targetY = from.targetY + (to.targetY - from.targetY) * e,
            targetZ = from.targetZ + (to.targetZ - from.targetZ) * e,
            distance = from.distance + (to.distance - from.distance) * e,
            pitch = from.pitch + (to.pitch - from.pitch) * e,
        )
    }

    fun orbit(camera: SynapseCamera, dx: Float, dy: Float): SynapseCamera =
        camera.copy(yaw = camera.yaw - dx * 0.008f, pitch = (camera.pitch + dy * 0.008f).coerceIn(-1.45f, 1.45f))

    fun pan(camera: SynapseCamera, dx: Float, dy: Float, height: Float): SynapseCamera {
        val back = eyeBack(camera)
        var rx = back[2]
        var rz = -back[0]
        val rl = hypot(rx, rz).takeIf { it > 1e-6f } ?: 1f
        rx /= rl
        rz /= rl
        val fx = -back[0]
        val fy = -back[1]
        val fz = -back[2]
        val ux = 0f * fz - rz * fy
        val uy = rz * fx - rx * fz
        val uz = rx * fy - 0f * fx
        val scale = 2.0f * camera.distance * tan(Math.toRadians(FOV_DEGREES * 0.5).toFloat()) / max(1f, height)
        return camera.copy(
            targetX = camera.targetX - rx * dx * scale + ux * dy * scale,
            targetY = camera.targetY + uy * dy * scale,
            targetZ = camera.targetZ - rz * dx * scale + uz * dy * scale,
        )
    }

    fun zoomAt(camera: SynapseCamera, factor: Float, x: Float, y: Float, width: Float, height: Float): SynapseCamera {
        val newDistance = (camera.distance * factor).coerceIn(12.0f, 2400.0f)
        val applied = newDistance / camera.distance
        val ndcX = x / max(1f, width) * 2.0f - 1.0f
        val ndcY = 1.0f - y / max(1f, height) * 2.0f
        val back = eyeBack(camera)
        val fx = -back[0]
        val fy = -back[1]
        val fz = -back[2]
        var rx = -fz
        var rz = fx
        val rl = hypot(rx, rz).takeIf { it > 1e-6f } ?: 1f
        rx /= rl
        rz /= rl
        val ux = 0f * fz - rz * fy
        val uy = rz * fx - rx * fz
        val uz = rx * fy - 0f * fx
        val tanV = tan(Math.toRadians(FOV_DEGREES * 0.5).toFloat())
        val tanH = tanV * max(1f, width) / max(1f, height)
        val underX = camera.targetX + (rx * ndcX * tanH + ux * ndcY * tanV) * camera.distance
        val underY = camera.targetY + (uy * ndcY * tanV) * camera.distance
        val underZ = camera.targetZ + (rz * ndcX * tanH + uz * ndcY * tanV) * camera.distance
        return camera.copy(
            targetX = camera.targetX + (underX - camera.targetX) * (1.0f - applied),
            targetY = camera.targetY + (underY - camera.targetY) * (1.0f - applied),
            targetZ = camera.targetZ + (underZ - camera.targetZ) * (1.0f - applied),
            distance = newDistance,
        )
    }

    fun pick(style: SynapseStyle, projection: SynapseProjection, x: Float, y: Float, reachPadding: Float = 1.5f, nearDistance: Float = 9.0f): Int {
        var best = -1
        var bestDepth = 0f
        var near = -1
        var nearBest = nearDistance
        val n = min(projection.size, style.nodeState.size)
        for (i in 0 until n) {
            if (!projection.onScreen[i] || style.nodeState[i] == HIDDEN) continue
            val d = hypot(projection.x[i] - x, projection.y[i] - y)
            val reach = max(projection.radius[i], 3.0f) + reachPadding
            if (d <= reach && (best < 0 || projection.depth[i] < bestDepth)) {
                best = i
                bestDepth = projection.depth[i]
            } else if (best < 0 && d < nearBest) {
                near = i
                nearBest = d
            }
        }
        if (best < 0) best = near
        return if (best >= 0) style.graph.nodes[best].file else -1
    }

    fun dust(shape: SynapseBrainShape, count: Int = 1800): FloatArray {
        val out = ArrayList<Float>()
        val p = FloatArray(3)
        for (i in 0 until count) {
            val y = 1.0f - 2.0f * (i.toFloat() + 0.5f) / count.toFloat()
            val r = sqrt(max(0.0f, 1.0f - y * y))
            val phi = i.toFloat() * 2.39996323f
            val dx = r * sin(phi)
            val dz = r * cos(phi)
            shape.onShell(dx, y, dz, shape.outerAt(dx, y, dz) + 0.025f, p)
            if (abs(p[0]) < shape.fissure * 0.6f) continue
            out.add(p[0])
            out.add(p[1])
            out.add(p[2])
        }
        return out.toFloatArray()
    }

    fun stars(count: Int = 420): FloatArray {
        val out = FloatArray(count * 4)
        for (i in 0 until count) {
            val a = Synapse.stableHash("star-a-$i")
            val b = Synapse.stableHash("star-b-$i")
            val c = Synapse.stableHash("star-c-$i")
            val z = (a and 0xffff) / 32767.5f - 1.0f
            val theta = (b and 0xffff) / 65535.0f * 6.2831853f
            val r = sqrt(max(0.0f, 1.0f - z * z))
            val bright = ((c and 0xffff) / 65535.0f).let { it * it * it }
            out[i * 4] = r * cos(theta)
            out[i * 4 + 1] = z
            out[i * 4 + 2] = r * sin(theta)
            out[i * 4 + 3] = bright
        }
        return out
    }

    fun phaseOf(seed: Int): Float = (seed and 0xffff) / 65535.0f

    fun shortLobeName(name: String): String {
        val parts = name.split('/').filter { it.isNotEmpty() }
        return if (parts.size <= 2) name else parts.subList(parts.size - 2, parts.size).joinToString("/")
    }

    fun graphCycleOf(graph: SynapseGraph, snapshotCycle: Int): Int = graph.cycles.indexOfFirst { it.index == snapshotCycle }

    fun cycleOrder(graph: SynapseGraph): List<Int> =
        graph.cycles.indices.sortedWith(compareByDescending<Int> { graph.cycles[it].files.size }.thenBy { it })

    fun searchMatches(snapshot: InsightSnapshot, query: String, limit: Int, graph: SynapseGraph? = null): List<Int> {
        val needle = query.trim()
        if (needle.isEmpty() || limit <= 0) return emptyList()
        val exact = snapshot.indexOf(needle)
        val out = ArrayList<Int>()
        if (exact >= 0) out.add(exact)
        val linked = ArrayList<Int>()
        val other = ArrayList<Int>()
        for ((i, file) in snapshot.files.withIndex()) {
            if (i == exact || !file.path.contains(needle, ignoreCase = true)) continue
            if (graph == null || graph.nodeFor(i) >= 0) linked.add(i) else other.add(i)
        }
        for (i in linked) {
            if (out.size >= limit) break
            out.add(i)
        }
        for (i in other) {
            if (out.size >= limit) break
            out.add(i)
        }
        return out
    }
}
