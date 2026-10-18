package com.forkmesh.nativeapp.codeinsight

enum class SynapseDirection { Imports, Importers, Both }

data class SynapseBrainShape(
    val halfWidth: Float = 96.0f,
    val halfHeightTop: Float = 76.0f,
    val halfHeightBottom: Float = 58.0f,
    val halfLength: Float = 124.0f,
    val fissure: Float = 15.0f,
    val innerShell: Float = 0.4f,
    val outerShell: Float = 0.96f,
) {
    val boundingRadius: Float get() = halfLength

    fun radiusOf(x: Float, y: Float, z: Float): Float {
        val length = SynapseMath.length(x, y, z)
        if (length < 1e-6f) return 0f
        val taper = 1.0f - 0.1f * (z / length)
        val a = halfWidth * taper
        val b = if (y >= 0) halfHeightTop else halfHeightBottom
        val px = x / a
        val py = y / b
        val pz = z / halfLength
        return kotlin.math.sqrt(px * px + py * py + pz * pz)
    }

    fun outerAt(x: Float, y: Float, z: Float): Float {
        val length = SynapseMath.length(x, y, z)
        if (length < 1e-6f) return outerShell
        val dx = x / length
        val dy = y / length
        val dz = z / length
        val theta = SynapseMath.atan2(dy, dz)
        val phi = SynapseMath.asin(dx.coerceIn(-1.0f, 1.0f))
        val groove = SynapseMath.sin(10.0f * theta + 2.6f * SynapseMath.sin(5.0f * phi + 1.3f * theta))
        return outerShell - 0.055f * (0.5f + 0.5f * groove)
    }

    fun onShell(x: Float, y: Float, z: Float, radius: Float, out: FloatArray, at: Int = 0) {
        val r = radiusOf(x, y, z)
        if (r < 1e-6f) {
            out[at] = 0f
            out[at + 1] = 0f
            out[at + 2] = 0f
            return
        }
        val scale = radius / r
        out[at] = x * scale
        out[at + 1] = y * scale
        out[at + 2] = z * scale
    }
}

class SynapseLobe(
    val name: String,
    val path: String,
    val layer: Int,
    var anchorX: Float = 0f,
    var anchorY: Float = 0f,
    var anchorZ: Float = 0f,
    var hemisphere: Int = 1,
    val nodes: MutableList<Int> = ArrayList(),
)

class SynapseNode(
    val file: Int,
    val lobe: Int,
    val seed: Int,
    val cycle: Boolean,
    var hemisphere: Int = 1,
    var size: Float = 1.0f,
    var fanIn: Int = 0,
    var fanOut: Int = 0,
)

class SynapseEdge(val from: Int, val to: Int, val cycle: Boolean, val seed: Int)

class SynapseCycleInfo(
    val index: Int,
    val files: List<Int>,
    val path: List<Int>,
    val edges: MutableList<Int> = ArrayList(),
)

class SynapseGraph(
    val snapshot: InsightSnapshot,
    val includesIsolated: Boolean,
    val shape: SynapseBrainShape,
    val nodes: List<SynapseNode>,
    val edges: List<SynapseEdge>,
    val nodeOfFile: IntArray,
    val lobes: List<SynapseLobe>,
    val outEdges: List<IntArray>,
    val inEdges: List<IntArray>,
    val cycles: List<SynapseCycleInfo>,
    val imports: Int,
    val spacing: Float,
) {
    fun nodeFor(file: Int): Int = if (file >= 0 && file < nodeOfFile.size) nodeOfFile[file] else -1
}

data class SynapseLayoutOptions(
    val iterations: Int = 300,
    val barnesHutAbove: Int = 1500,
    val theta: Float = 0.85f,
    val publishIntervalMs: Long = 45,
)

class SynapseNeighbourhood(
    val imports: Map<Int, Int> = emptyMap(),
    val importers: Map<Int, Int> = emptyMap(),
) {
    fun contains(file: Int): Boolean = imports.containsKey(file) || importers.containsKey(file)

    val size: Int get() = imports.size + importers.size
}
