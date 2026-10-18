package com.forkmesh.nativeapp.codeinsight

import java.util.concurrent.Callable
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors
import kotlin.math.abs
import kotlin.math.sqrt

internal object SynapseLayout {
    private const val GOLDEN_ANGLE = 2.39996323f

    private class Grouping(
        val groupOfFile: IntArray,
        val paths: List<String>,
        val names: List<String>,
        val layers: List<Int>,
    )

    private fun directoryKey(path: String, depth: Int): String {
        val parts = path.split('/').filter { it.isNotEmpty() }
        if (parts.size <= 1) return ""
        val take = minOf(depth, parts.size - 1)
        return parts.subList(0, take).joinToString("/")
    }

    private fun fallbackGrouping(snapshot: InsightSnapshot, files: List<Int>): Grouping {
        val wanted = (files.size / 40).coerceIn(3, 14)
        var chosen = 1
        for (depth in 1..4) {
            val keys = HashSet<String>()
            for (file in files) keys.add(directoryKey(snapshot.files[file].path, depth))
            chosen = depth
            if (keys.size >= wanted) break
        }
        val groupOfFile = IntArray(snapshot.files.size) { -1 }
        val index = HashMap<String, Int>()
        val paths = ArrayList<String>()
        val names = ArrayList<String>()
        val layers = ArrayList<Int>()
        for (file in files.sortedBy { snapshot.files[it].path }) {
            val key = directoryKey(snapshot.files[file].path, chosen)
            val group = index.getOrPut(key) {
                paths.add(key)
                names.add(key.ifEmpty { "(root)" })
                layers.add(0)
                paths.size - 1
            }
            groupOfFile[file] = group
        }
        return Grouping(groupOfFile, paths, names, layers)
    }

    private fun moduleGrouping(snapshot: InsightSnapshot, files: List<Int>): Grouping {
        val modules = Insight.buildModuleGraph(snapshot, 0)
        if (modules.moduleOfFile.size != snapshot.files.size || modules.nodes.isEmpty()) return fallbackGrouping(snapshot, files)
        val groupOfFile = IntArray(snapshot.files.size) { -1 }
        val remap = HashMap<Int, Int>()
        val paths = ArrayList<String>()
        val names = ArrayList<String>()
        val layers = ArrayList<Int>()
        for (file in files) {
            val module = modules.moduleOfFile[file]
            if (module < 0 || module >= modules.nodes.size) return fallbackGrouping(snapshot, files)
            val group = remap.getOrPut(module) {
                val node = modules.nodes[module]
                paths.add(node.path)
                val name = node.path.ifEmpty { node.name }
                names.add(name.ifEmpty { "(root)" })
                layers.add(node.layer)
                paths.size - 1
            }
            groupOfFile[file] = group
        }
        return Grouping(groupOfFile, paths, names, layers)
    }

    private fun linked(file: InsightFile, self: Int): Boolean =
        file.dependsOn.any { it != self } || file.dependedOnBy.any { it != self }

    fun buildGraph(snapshot: InsightSnapshot, includeIsolated: Boolean, shape: SynapseBrainShape = SynapseBrainShape()): SynapseGraph {
        val nodeOfFile = IntArray(snapshot.files.size) { -1 }
        val files = ArrayList<Int>()
        for ((i, file) in snapshot.files.withIndex()) if (includeIsolated || linked(file, i)) files.add(i)
        val grouping = moduleGrouping(snapshot, files)
        val lobes = grouping.paths.indices.map { SynapseLobe(name = grouping.names[it], path = grouping.paths[it], layer = grouping.layers[it]) }
        val sorted = files.sortedBy { snapshot.files[it].path }
        val nodes = ArrayList<SynapseNode>(sorted.size)
        for (file in sorted) {
            val info = snapshot.files[file]
            val node = SynapseNode(
                file = file,
                lobe = maxOf(0, grouping.groupOfFile[file]),
                seed = Synapse.stableHash(info.path),
                cycle = info.cycle >= 0,
            )
            nodeOfFile[file] = nodes.size
            lobes[node.lobe].nodes.add(nodes.size)
            nodes.add(node)
        }
        val outLists = List(nodes.size) { ArrayList<Int>() }
        val inLists = List(nodes.size) { ArrayList<Int>() }
        val edges = ArrayList<SynapseEdge>()
        for (n in nodes.indices) {
            val info = snapshot.files[nodes[n].file]
            for (target in info.dependsOn.toSortedSet()) {
                val to = if (target >= 0 && target < nodeOfFile.size) nodeOfFile[target] else -1
                if (to < 0 || to == n) continue
                val edge = SynapseEdge(
                    from = n,
                    to = to,
                    cycle = info.cycle >= 0 && info.cycle == snapshot.files[target].cycle,
                    seed = SynapseMath.mixHash(nodes[n].seed xor (nodes[to].seed * 31)),
                )
                outLists[n].add(edges.size)
                inLists[to].add(edges.size)
                edges.add(edge)
            }
        }
        for ((n, node) in nodes.withIndex()) {
            node.fanIn = inLists[n].size
            node.fanOut = outLists[n].size
            val info = snapshot.files[node.file]
            val loc = maxOf(info.codeLines, info.lines / 2).toFloat()
            node.size = (0.55f + 0.1f * SynapseMath.log1p(loc) + 0.2f * sqrt(node.fanIn.toFloat())).coerceIn(0.5f, 3.2f)
        }
        val cycles = Synapse.cycleInfos(snapshot)
        val cycleOfInfo = HashMap<Int, Int>()
        for ((c, info) in cycles.withIndex()) cycleOfInfo[info.index] = c
        for ((e, edge) in edges.withIndex()) {
            if (!edge.cycle) continue
            val c = cycleOfInfo[snapshot.files[nodes[edge.from].file].cycle] ?: -1
            if (c >= 0) cycles[c].edges.add(e)
        }
        val volume = 4.18879f * shape.halfWidth * shape.halfLength * 0.5f * (shape.halfHeightTop + shape.halfHeightBottom) *
            (1.0f - SynapseMath.pow(shape.innerShell, 3.0f))
        val spacing = SynapseMath.cbrt(volume / maxOf(1, nodes.size).toFloat()).coerceIn(2.0f, 40.0f) * 0.9f
        placeLobes(lobes, nodes, edges, shape)
        for (node in nodes) {
            val lobe = lobes[node.lobe]
            node.hemisphere = if (lobe.hemisphere != 0) lobe.hemisphere else if ((node.seed ushr 7) and 1 != 0) 1 else -1
        }
        return SynapseGraph(
            snapshot = snapshot,
            includesIsolated = includeIsolated,
            shape = shape,
            nodes = nodes,
            edges = edges,
            nodeOfFile = nodeOfFile,
            lobes = lobes,
            outEdges = outLists.map { it.toIntArray() },
            inEdges = inLists.map { it.toIntArray() },
            cycles = cycles,
            imports = edges.size,
            spacing = spacing,
        )
    }

    private fun normalizeInPlace(v: FloatArray) {
        val len = SynapseMath.length(v[0], v[1], v[2])
        if (SynapseMath.fuzzyIsNull(len - 1.0f) || SynapseMath.fuzzyIsNull(len)) return
        v[0] /= len
        v[1] /= len
        v[2] /= len
    }

    private fun placeLobes(lobes: List<SynapseLobe>, nodes: List<SynapseNode>, edges: List<SynapseEdge>, shape: SynapseBrainShape) {
        val count = lobes.size
        if (count == 0) return
        if (count == 1) {
            lobes[0].anchorX = 0f
            lobes[0].anchorY = 0f
            lobes[0].anchorZ = 0f
            lobes[0].hemisphere = 0
            return
        }
        val order = (0 until count).sortedWith { a, b ->
            val la = lobes[a]
            val lb = lobes[b]
            when {
                la.layer != lb.layer -> la.layer.compareTo(lb.layer)
                la.nodes.size != lb.nodes.size -> lb.nodes.size.compareTo(la.nodes.size)
                else -> la.path.compareTo(lb.path)
            }
        }
        val points = Array(count) { i ->
            val y = 1.0f - 2.0f * (i.toFloat() + 0.5f) / count.toFloat()
            val r = sqrt(maxOf(0.0f, 1.0f - y * y))
            val phi = i.toFloat() * GOLDEN_ANGLE
            floatArrayOf(r * SynapseMath.sin(phi), y * 0.8f, r * SynapseMath.cos(phi))
        }
        points.sortWith { a, b ->
            when {
                a[2] < b[2] -> -1
                a[2] > b[2] -> 1
                a[0] < b[0] -> -1
                a[0] > b[0] -> 1
                else -> 0
            }
        }
        val ax = FloatArray(count)
        val ay = FloatArray(count)
        val az = FloatArray(count)
        for (i in 0 until count) {
            val target = order[i]
            ax[target] = points[i][0]
            ay[target] = points[i][1]
            az[target] = points[i][2]
        }
        val weights = HashMap<Long, Int>()
        for (edge in edges) {
            val a = nodes[edge.from].lobe
            val b = nodes[edge.to].lobe
            if (a == b) continue
            val key = (minOf(a, b).toLong() shl 32) or maxOf(a, b).toLong()
            weights[key] = (weights[key] ?: 0) + 1
        }
        val springKeys = weights.keys.sorted()
        val springA = IntArray(springKeys.size) { (springKeys[it] ushr 32).toInt() }
        val springB = IntArray(springKeys.size) { (springKeys[it] and 0xffffffffL).toInt() }
        val springW = FloatArray(springKeys.size) { SynapseMath.log1p(weights.getValue(springKeys[it]).toFloat()) }
        val mass = FloatArray(count)
        var totalMass = 0f
        for (i in 0 until count) {
            mass[i] = sqrt(lobes[i].nodes.size.toFloat() + 1.0f)
            totalMass += mass[i]
        }
        val ideal = 2.0f * sqrt(4.0f * 3.14159f / count.toFloat())
        val fx = FloatArray(count)
        val fy = FloatArray(count)
        val fz = FloatArray(count)
        for (iteration in 0 until 160) {
            val temp = 0.12f * (1.0f - iteration.toFloat() / 160.0f) + 0.004f
            fx.fill(0f)
            fy.fill(0f)
            fz.fill(0f)
            for (i in 0 until count) {
                for (j in i + 1 until count) {
                    val dx = ax[i] - ax[j]
                    val dy = ay[i] - ay[j]
                    val dz = az[i] - az[j]
                    val dist2 = maxOf(dx * dx + dy * dy + dz * dz, 1e-4f)
                    val strength = ideal * ideal * mass[i] * mass[j] / (totalMass / count.toFloat()) / (totalMass / count.toFloat())
                    val s = strength / dist2
                    val gx = dx * s * 0.05f
                    val gy = dy * s * 0.05f
                    val gz = dz * s * 0.05f
                    fx[i] += gx
                    fy[i] += gy
                    fz[i] += gz
                    fx[j] -= gx
                    fy[j] -= gy
                    fz[j] -= gz
                }
            }
            for (s in springKeys.indices) {
                val a = springA[s]
                val b = springB[s]
                val k = 0.04f * springW[s]
                val gx = (ax[b] - ax[a]) * k
                val gy = (ay[b] - ay[a]) * k
                val gz = (az[b] - az[a]) * k
                fx[a] += gx
                fy[a] += gy
                fz[a] += gz
                fx[b] -= gx
                fy[b] -= gy
                fz[b] -= gz
            }
            for (i in 0 until count) {
                var gx = fx[i]
                var gy = fy[i]
                var gz = fz[i]
                val len = SynapseMath.length(gx, gy, gz)
                if (len > temp) {
                    val scale = temp / len
                    gx *= scale
                    gy *= scale
                    gz *= scale
                }
                val px = ax[i] + gx
                val py = ay[i] + gy
                val pz = az[i] + gz
                val plen = SynapseMath.length(px, py, pz)
                if (plen > 1e-5f) {
                    ax[i] = px / plen
                    ay[i] = py / plen
                    az[i] = pz / plen
                } else {
                    ax[i] = 0f
                    ay[i] = 1f
                    az[i] = 0f
                }
            }
        }
        val middle = 0.5f * (shape.innerShell + shape.outerShell)
        val direction = FloatArray(3)
        val anchor = FloatArray(3)
        for (i in 0 until count) {
            direction[0] = ax[i]
            direction[1] = ay[i]
            direction[2] = az[i]
            if (abs(direction[0]) < 0.25f) {
                val sign = if (direction[0] < 0) -1.0f else if (direction[0] > 0) 1.0f else if (i % 2 == 0) 1.0f else -1.0f
                direction[0] = 0.25f * sign
                normalizeInPlace(direction)
            }
            if (direction[1] < -0.6f) {
                direction[1] = -0.6f
                normalizeInPlace(direction)
            }
            shape.onShell(direction[0], direction[1], direction[2], middle, anchor)
            val lobe = lobes[i]
            lobe.anchorX = anchor[0]
            lobe.anchorY = anchor[1]
            lobe.anchorZ = anchor[2]
            lobe.hemisphere = if (anchor[0] < 0) -1 else 1
        }
    }

    fun seedPositions(graph: SynapseGraph): FloatArray {
        val n = graph.nodes.size
        val positions = FloatArray(n * 3)
        val shape = graph.shape
        val jitter = FloatArray(3)
        val shell = FloatArray(3)
        for (i in 0 until n) {
            val node = graph.nodes[i]
            val lobe = graph.lobes[node.lobe]
            val spread = graph.spacing * SynapseMath.cbrt(lobe.nodes.size.toFloat() + 1.0f) * 0.7f
            var px: Float
            var py: Float
            var pz: Float
            val hemisphere = node.hemisphere.toFloat()
            SynapseMath.jitter(node.seed, jitter)
            if (graph.lobes.size == 1) {
                var dx = jitter[0]
                var dy = jitter[1]
                var dz = jitter[2]
                if (dx * dx + dy * dy + dz * dz < 1e-4f) {
                    dx = 0f
                    dy = 1f
                    dz = 0f
                }
                dx = abs(dx) * hemisphere + 0.15f * hemisphere
                val len = SynapseMath.length(dx, dy, dz)
                if (!SynapseMath.fuzzyIsNull(len - 1.0f)) {
                    if (SynapseMath.fuzzyIsNull(len)) {
                        dx = 0f
                        dy = 0f
                        dz = 0f
                    } else {
                        dx /= len
                        dy /= len
                        dz /= len
                    }
                }
                shape.onShell(dx, dy, dz, 0.55f + 0.35f * SynapseMath.unitFromHash(SynapseMath.mixHash(node.seed)), shell)
                px = shell[0]
                py = shell[1]
                pz = shell[2]
            } else {
                px = lobe.anchorX + jitter[0] * spread
                py = lobe.anchorY + jitter[1] * spread
                pz = lobe.anchorZ + jitter[2] * spread
            }
            if (px * hemisphere < shape.fissure) px = hemisphere * (shape.fissure + SynapseMath.unitFromHash(node.seed) * 2.0f)
            val r = shape.radiusOf(px, py, pz)
            val outer = shape.outerAt(px, py, pz)
            if (r > outer) {
                val scale = outer / r
                px *= scale
                py *= scale
                pz *= scale
            }
            positions[i * 3] = px
            positions[i * 3 + 1] = py
            positions[i * 3 + 2] = pz
        }
        return positions
    }

    fun layoutIterations(graph: SynapseGraph, options: SynapseLayoutOptions): Int {
        val n = graph.nodes.size
        if (n <= 5000) return options.iterations
        return maxOf(options.iterations * 2 / 5, options.iterations * 5000 / n)
    }

    fun lobeCentres(graph: SynapseGraph, positions: FloatArray): FloatArray {
        val centres = FloatArray(graph.lobes.size * 3)
        for ((l, lobe) in graph.lobes.withIndex()) {
            if (lobe.nodes.isEmpty() || positions.size != graph.nodes.size * 3) {
                centres[l * 3] = lobe.anchorX
                centres[l * 3 + 1] = lobe.anchorY
                centres[l * 3 + 2] = lobe.anchorZ
                continue
            }
            var sx = 0f
            var sy = 0f
            var sz = 0f
            for (n in lobe.nodes) {
                sx += positions[n * 3]
                sy += positions[n * 3 + 1]
                sz += positions[n * 3 + 2]
            }
            val count = lobe.nodes.size.toFloat()
            centres[l * 3] = sx / count
            centres[l * 3 + 1] = sy / count
            centres[l * 3 + 2] = sz / count
        }
        return centres
    }

    private fun shellNormal(shape: SynapseBrainShape, x: Float, y: Float, z: Float, out: FloatArray) {
        val b = if (y >= 0) shape.halfHeightTop else shape.halfHeightBottom
        val nx = x / (shape.halfWidth * shape.halfWidth)
        val ny = y / (b * b)
        val nz = z / (shape.halfLength * shape.halfLength)
        val len = SynapseMath.length(nx, ny, nz)
        if (len > 1e-6f) {
            out[0] = nx / len
            out[1] = ny / len
            out[2] = nz / len
        } else {
            out[0] = 0f
            out[1] = 1f
            out[2] = 0f
        }
    }

    fun runLayout(
        graph: SynapseGraph,
        options: SynapseLayoutOptions,
        publish: ((FloatArray, Double) -> Unit)?,
        stop: (() -> Boolean)?,
    ): FloatArray {
        val positions = seedPositions(graph)
        val n = graph.nodes.size
        if (n == 0) {
            publish?.invoke(positions.copyOf(), 1.0)
            return positions
        }
        val shape = graph.shape
        val k = graph.spacing
        val k2 = k * k
        val theta2 = options.theta * options.theta
        val iterations = maxOf(1, layoutIterations(graph, options))
        val barnesHut = n > options.barnesHutAbove
        val lobePull = graph.lobes.size > 1
        val startTemp = 0.06f * shape.halfLength + 1.5f * k
        val endTemp = 0.02f * k
        val chunkSize = 256
        val chunks = (0 until n step chunkSize).map { it to minOf(n, it + chunkSize) }
        val force = FloatArray(n * 3)
        val normal = FloatArray(3)
        val edgeFrom = IntArray(graph.edges.size) { graph.edges[it].from }
        val edgeTo = IntArray(graph.edges.size) { graph.edges[it].to }
        val workers = if (n > 300) maxOf(1, minOf(chunks.size, Runtime.getRuntime().availableProcessors())) else 1
        val pool = if (workers > 1) {
            Executors.newFixedThreadPool(workers) { runnable -> Thread(runnable, "SynapseLayout").apply { isDaemon = true } }
        } else {
            null
        }
        try {
            var lastPublish = System.nanoTime()
            publish?.invoke(positions.copyOf(), 0.0)
            for (iteration in 0 until iterations) {
                if (stop != null && stop()) return positions
                val progress = iteration.toFloat() / iterations.toFloat()
                val remaining = 1.0f - progress
                val temp = startTemp * (remaining * remaining) + endTemp
                val tree = if (barnesHut) SynapseOctree(positions, n) else null
                val repulse = { begin: Int, end: Int ->
                    val stack = if (tree != null) IntArray(512) else null
                    for (i in begin until end) {
                        if (tree != null) {
                            tree.force(i, k2, theta2, force, stack!!)
                            continue
                        }
                        val px = positions[i * 3]
                        val py = positions[i * 3 + 1]
                        val pz = positions[i * 3 + 2]
                        var tx = 0f
                        var ty = 0f
                        var tz = 0f
                        for (j in 0 until n) {
                            if (j == i) continue
                            val dx = px - positions[j * 3]
                            val dy = py - positions[j * 3 + 1]
                            val dz = pz - positions[j * 3 + 2]
                            val dist2 = maxOf(dx * dx + dy * dy + dz * dz, 0.01f * k2)
                            val s = k2 / dist2
                            tx += dx * s
                            ty += dy * s
                            tz += dz * s
                        }
                        force[i * 3] = tx
                        force[i * 3 + 1] = ty
                        force[i * 3 + 2] = tz
                    }
                }
                if (pool != null) {
                    val futures = pool.invokeAll(chunks.map { (begin, end) -> Callable { repulse(begin, end) } })
                    for (future in futures) {
                        try {
                            future.get()
                        } catch (failure: ExecutionException) {
                            throw failure.cause ?: failure
                        }
                    }
                } else {
                    for ((begin, end) in chunks) repulse(begin, end)
                }
                for (e in edgeFrom.indices) {
                    val from = edgeFrom[e]
                    val to = edgeTo[e]
                    val dx = positions[to * 3] - positions[from * 3]
                    val dy = positions[to * 3 + 1] - positions[from * 3 + 1]
                    val dz = positions[to * 3 + 2] - positions[from * 3 + 2]
                    val dist = SynapseMath.length(dx, dy, dz)
                    if (dist < 1e-4f) continue
                    val magnitude = 1.1f * k * SynapseMath.log(maxOf(dist, 0.3f * k) / k)
                    val s = magnitude / dist
                    val gx = dx * s
                    val gy = dy * s
                    val gz = dz * s
                    force[from * 3] += gx
                    force[from * 3 + 1] += gy
                    force[from * 3 + 2] += gz
                    force[to * 3] -= gx
                    force[to * 3 + 1] -= gy
                    force[to * 3 + 2] -= gz
                }
                for (i in 0 until n) {
                    val node = graph.nodes[i]
                    val hemisphere = node.hemisphere.toFloat()
                    var px = positions[i * 3]
                    var py = positions[i * 3 + 1]
                    var pz = positions[i * 3 + 2]
                    var fx = force[i * 3]
                    var fy = force[i * 3 + 1]
                    var fz = force[i * 3 + 2]
                    if (lobePull) {
                        val lobe = graph.lobes[node.lobe]
                        fx += (lobe.anchorX - px) * 0.05f
                        fy += (lobe.anchorY - py) * 0.05f
                        fz += (lobe.anchorZ - pz) * 0.05f
                    }
                    val r = shape.radiusOf(px, py, pz)
                    shellNormal(shape, px, py, pz, normal)
                    val outer = shape.outerAt(px, py, pz)
                    if (r > outer) {
                        val s = (r - outer) * shape.halfLength * 2.5f
                        fx -= normal[0] * s
                        fy -= normal[1] * s
                        fz -= normal[2] * s
                    } else if (r < shape.innerShell) {
                        val s = (shape.innerShell - r) * shape.halfLength * 2.0f
                        fx += normal[0] * s
                        fy += normal[1] * s
                        fz += normal[2] * s
                    }
                    val side = px * hemisphere
                    if (side < shape.fissure) fx = fx + hemisphere * (shape.fissure - side) * 2.0f
                    val len = SynapseMath.length(fx, fy, fz)
                    if (len > 1e-6f) {
                        val s = minOf(len, temp) / len
                        px += fx * s
                        py += fy * s
                        pz += fz * s
                    }
                    val after = shape.radiusOf(px, py, pz)
                    if (after > 0.99f) {
                        val s = 0.99f / after
                        px *= s
                        py *= s
                        pz *= s
                    }
                    if (px * hemisphere < 0.5f * shape.fissure) px = hemisphere * 0.5f * shape.fissure
                    positions[i * 3] = px
                    positions[i * 3 + 1] = py
                    positions[i * 3 + 2] = pz
                }
                if (publish != null && System.nanoTime() - lastPublish >= options.publishIntervalMs * 1_000_000L) {
                    publish(positions.copyOf(), (iteration + 1).toDouble() / iterations.toDouble())
                    lastPublish = System.nanoTime()
                }
            }
        } finally {
            pool?.shutdownNow()
        }
        publish?.invoke(positions.copyOf(), 1.0)
        return positions
    }
}

internal class SynapseOctree(private val points: FloatArray, private val n: Int) {
    private val order = IntArray(n) { it }
    private val scratch = IntArray(n)
    private var count = 0
    private var centreX = FloatArray(maxOf(16, n * 2))
    private var centreY = FloatArray(centreX.size)
    private var centreZ = FloatArray(centreX.size)
    private var mass = FloatArray(centreX.size)
    private var size = FloatArray(centreX.size)
    private var children = IntArray(centreX.size * 8)
    private var begins = IntArray(centreX.size)
    private var ends = IntArray(centreX.size)
    private var leaf = BooleanArray(centreX.size)

    init {
        if (n > 0) {
            var lx = points[0]
            var ly = points[1]
            var lz = points[2]
            var hx = lx
            var hy = ly
            var hz = lz
            for (i in 0 until n) {
                val x = points[i * 3]
                val y = points[i * 3 + 1]
                val z = points[i * 3 + 2]
                lx = minOf(lx, x)
                ly = minOf(ly, y)
                lz = minOf(lz, z)
                hx = maxOf(hx, x)
                hy = maxOf(hy, y)
                hz = maxOf(hz, z)
            }
            val span = maxOf(maxOf(hx - lx, hy - ly), maxOf(hz - lz, 1e-3f)) * 1.001f
            build(0, n, (lx + hx) * 0.5f, (ly + hy) * 0.5f, (lz + hz) * 0.5f, span, 0)
        }
    }

    private fun grow() {
        val capacity = centreX.size * 2
        centreX = centreX.copyOf(capacity)
        centreY = centreY.copyOf(capacity)
        centreZ = centreZ.copyOf(capacity)
        mass = mass.copyOf(capacity)
        size = size.copyOf(capacity)
        children = children.copyOf(capacity * 8)
        begins = begins.copyOf(capacity)
        ends = ends.copyOf(capacity)
        leaf = leaf.copyOf(capacity)
    }

    private fun octant(body: Int, cx: Float, cy: Float, cz: Float): Int =
        (if (points[body * 3] >= cx) 1 else 0) or (if (points[body * 3 + 1] >= cy) 2 else 0) or (if (points[body * 3 + 2] >= cz) 4 else 0)

    private fun build(begin: Int, end: Int, bx: Float, by: Float, bz: Float, span: Float, depth: Int): Int {
        if (count == centreX.size) grow()
        val index = count++
        var sx = 0f
        var sy = 0f
        var sz = 0f
        for (i in begin until end) {
            val body = order[i]
            sx += points[body * 3]
            sy += points[body * 3 + 1]
            sz += points[body * 3 + 2]
        }
        val cellMass = (end - begin).toFloat()
        val divisor = maxOf(1.0f, cellMass)
        centreX[index] = sx / divisor
        centreY[index] = sy / divisor
        centreZ[index] = sz / divisor
        mass[index] = cellMass
        size[index] = span
        begins[index] = begin
        ends[index] = end
        for (o in 0 until 8) children[index * 8 + o] = -1
        if (end - begin <= 4 || depth >= 24) {
            leaf[index] = true
            return index
        }
        leaf[index] = false
        val counts = IntArray(8)
        for (i in begin until end) counts[octant(order[i], bx, by, bz)]++
        val starts = IntArray(9)
        starts[0] = begin
        for (o in 0 until 8) starts[o + 1] = starts[o] + counts[o]
        val cursor = starts.copyOf(8)
        for (i in begin until end) {
            val body = order[i]
            scratch[cursor[octant(body, bx, by, bz)]++] = body
        }
        for (i in begin until end) order[i] = scratch[i]
        val half = span * 0.5f
        val quarter = span * 0.25f
        for (o in 0 until 8) {
            if (counts[o] == 0) continue
            val child = build(
                starts[o],
                starts[o + 1],
                bx + if (o and 1 != 0) quarter else -quarter,
                by + if (o and 2 != 0) quarter else -quarter,
                bz + if (o and 4 != 0) quarter else -quarter,
                half,
                depth + 1,
            )
            children[index * 8 + o] = child
        }
        return index
    }

    fun force(body: Int, k2: Float, theta2: Float, out: FloatArray, local: IntArray) {
        var tx = 0f
        var ty = 0f
        var tz = 0f
        if (count > 0) {
            val px = points[body * 3]
            val py = points[body * 3 + 1]
            val pz = points[body * 3 + 2]
            var top = 0
            local[top++] = 0
            while (top > 0) {
                val cell = local[--top]
                if (leaf[cell]) {
                    for (i in begins[cell] until ends[cell]) {
                        val other = order[i]
                        if (other == body) continue
                        val dx = px - points[other * 3]
                        val dy = py - points[other * 3 + 1]
                        val dz = pz - points[other * 3 + 2]
                        val dist2 = maxOf(dx * dx + dy * dy + dz * dz, 0.01f * k2)
                        val s = k2 / dist2
                        tx += dx * s
                        ty += dy * s
                        tz += dz * s
                    }
                    continue
                }
                val dx = px - centreX[cell]
                val dy = py - centreY[cell]
                val dz = pz - centreZ[cell]
                val dist2 = dx * dx + dy * dy + dz * dz
                if (size[cell] * size[cell] < theta2 * dist2) {
                    val s = mass[cell] * k2 / maxOf(dist2, 0.01f * k2)
                    tx += dx * s
                    ty += dy * s
                    tz += dz * s
                    continue
                }
                for (o in 0 until 8) {
                    val child = children[cell * 8 + o]
                    if (child >= 0 && top < 511) local[top++] = child
                }
            }
        }
        out[body * 3] = tx
        out[body * 3 + 1] = ty
        out[body * 3 + 2] = tz
    }
}
