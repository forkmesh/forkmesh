package com.forkmesh.nativeapp.chat.face

typealias CallFaceRegions = Map<String, List<List<Double>>>

data class CallFaceFrame(
    val left: CallFacePoint,
    val right: CallFacePoint,
    val mid: CallFacePoint,
    val span: Double,
    val ux: CallFacePoint,
    val uy: CallFacePoint,
    val roll: Double,
)

data class CallFaceAnchors(
    val eyeL: CallFacePoint,
    val eyeR: CallFacePoint,
    val mouth: CallFacePoint? = null,
    val mouthWidth: Double? = null,
    val open: Double? = null,
    val yaw: Double? = null,
)

data class CallFacePose(
    val scale: Double,
    val rows: List<DoubleArray>,
    val t: DoubleArray,
    val roll: Double,
    val yaw: Double,
    val pitch: Double,
    val depth: Boolean,
)

data class CallFaceTriangle(val v: IntArray, val hole: DoubleArray, val mouth: Boolean, val cull: Boolean)

data class CallFaceHeadMesh(val triangles: List<CallFaceTriangle>, val count: Int, val extraStart: Int, val mouthStart: Int)

data class CallFaceLiveHead(val points: List<CallFacePoint>, val pose: CallFacePose, val jawTrust: DoubleArray)

object CallFaceGeometry {
    const val LANDMARK_COUNT = 68
    val jaw: List<Int> = (0 until 17).toList()
    val leftEye: List<Int> = (36 until 42).toList()
    val rightEye: List<Int> = (42 until 48).toList()
    val outerLips: List<Int> = (48 until 60).toList()
    val innerLips: List<Int> = (60 until 68).toList()
    const val FOREHEAD_LIFT = 0.5
    const val RING_START = 78

    internal val mirror = intArrayOf(
        16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
        26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
        27, 28, 29, 30, 35, 34, 33, 32, 31,
        45, 44, 43, 42, 47, 46, 39, 38, 37, 36, 41, 40,
        54, 53, 52, 51, 50, 49, 48, 59, 58, 57, 56, 55,
        64, 63, 62, 61, 60, 67, 66, 65,
    )

    internal val depthTable = doubleArrayOf(
        -0.95, -0.9, -0.8, -0.65, -0.45, -0.25, -0.08, 0.05, 0.1, 0.05, -0.08, -0.25, -0.45, -0.65, -0.8, -0.9, -0.95,
        -0.25, -0.08, 0.02, 0.05, 0.05, 0.05, 0.05, 0.02, -0.08, -0.25,
        0.0, 0.12, 0.25, 0.38, 0.12, 0.2, 0.25, 0.2, 0.12,
        -0.12, -0.04, -0.04, -0.03, -0.04, -0.04, -0.03, -0.04, -0.04, -0.12, -0.04, -0.04,
        0.02, 0.12, 0.17, 0.19, 0.17, 0.12, 0.02, 0.12, 0.15, 0.16, 0.15, 0.12,
        0.04, 0.13, 0.14, 0.13, 0.04, 0.13, 0.14, 0.13,
    )

    val poseWeights: DoubleArray = DoubleArray(68) { i -> if (i <= 16) 0.4 else if (i <= 26) 0.8 else if (i <= 35) 1.4 else if (i <= 47) 1.4 else 0.0 }
    private const val RESIDUAL_SIGMA = 0.55
    private const val RESIDUAL_ANCHOR = 0.08
    private const val FOREHEAD_CURVE = 1.5
    private const val FOREHEAD_BACK = -0.12
    private const val RING_DEPTH = -1.0
    internal val headCenter = CallFacePoint(0.0, -0.2, -1.1)
    private const val SIDE_REACH = 0.4
    private const val SIDE_BACK = 0.35
    private const val JAW_FOLLOW = 0.7
    private const val JAW_FACING_LOW = 0.08
    private const val JAW_FACING_HIGH = 0.38
    private const val JAW_NORMAL_Z = 0.55
    private val fieldSources: List<Pair<Int, Double>> = (17 until 36).map { it to 1.0 } + listOf(36 to 1.0, 39 to 1.0, 42 to 1.0, 45 to 1.0)
    private val cheekSources: List<Pair<Int, Double>> = fieldSources + listOf(48 to 0.6, 54 to 0.6, 57 to 0.5)
    private val cheekSide: List<Pair<List<Pair<Int, Double>>, Double>> = listOf(
        listOf(2 to 0.5, 31 to 0.5) to 0.2,
        listOf(1 to 0.5, 41 to 0.5) to 0.16,
        listOf(4 to 0.5, 48 to 0.5) to 0.18,
        listOf(3 to 0.4, 31 to 0.3, 48 to 0.3) to 0.2,
        listOf(6 to 0.5, 59 to 0.5) to 0.1,
        listOf(40 to 0.5, 31 to 0.5) to 0.08,
        listOf(0 to 0.55, 36 to 0.45) to 0.1,
    )
    private val cheekMiddle: List<Pair<List<Pair<Int, Double>>, Double>> = listOf(listOf(8 to 0.5, 57 to 0.5) to 0.08)
    private val chinDrop = listOf(5 to 0.3, 6 to 0.6, 7 to 0.85, 8 to 1.0, 9 to 0.85, 10 to 0.6, 11 to 0.3)
    private const val MAX_OPEN = 0.42
    private const val NOSE_TRUST = 0.06
    private const val RING_GAP = 0.05
    private val nose = (27 until 36).toList()
    private const val NOSE_RIGID = 0.85
    private const val DEPTH_SPREAD = 0.04
    private const val LIP_STEPS = 4
    private const val MOUTH_FEATHER = 0.14
    private const val MOUTH_DEEP = 0.4
    internal val upperInner = listOf(60, 61, 62, 63, 64)
    internal val lowerInner = listOf(60, 67, 66, 65, 64)
    private val upperOuter = listOf(48, 49, 50, 51, 52, 53, 54)
    private val lowerOuter = listOf(48, 59, 58, 57, 56, 55, 54)
    private const val LIP_KEEP = 0.65
    private const val CORNER_TAPER = 0.35
    private val openProfile = listOf(55 to 0.45, 56 to 0.8, 57 to 1.0, 58 to 0.8, 59 to 0.45, 65 to 0.8, 66 to 1.0, 67 to 0.8)
    private val liftProfile = listOf(61 to 0.7, 62 to 1.0, 63 to 0.7)

    internal class Weights {
        val index = ArrayList<Int>()
        val weight = ArrayList<Double>()
        val size: Int get() = index.size

        fun add(i: Int, value: Double) {
            val at = index.indexOf(i)
            if (at >= 0) {
                weight[at] = weight[at] + value
            } else {
                index.add(i)
                weight.add(0.0 + value)
            }
        }

        companion object {
            fun of(vararg pairs: Pair<Int, Double>): Weights = Weights().also { w -> pairs.forEach { w.index.add(it.first); w.weight.add(it.second) } }
            fun of(pairs: List<Pair<Int, Double>>): Weights = of(*pairs.toTypedArray())
        }
    }

    internal class MouthRef(val landmark: Int?, val extra: Int?, val weights: Weights)

    internal class Mouth(
        val extras: List<Weights>,
        val upper: List<MouthRef>,
        val lower: List<MouthRef>,
        val innerUpper: List<MouthRef>,
        val innerLower: List<MouthRef>,
        val deepUpper: List<MouthRef>,
        val deepLower: List<MouthRef>,
        val contourCount: Int,
    )

    internal val mouth: Mouth = buildMouth()
    val MOUTH_EXTRA_COUNT: Int = mouth.extras.size
    private val cheeks: List<Pair<Weights, Double>> =
        (cheekSide + cheekSide.map { (weights, lift) -> weights.map { mirror[it.first] to it.second } to lift } + cheekMiddle)
            .map { (weights, lift) -> Weights.of(weights) to lift }
    val CHEEK_COUNT: Int = cheeks.size
    val EXTRA_COUNT: Int = CHEEK_COUNT + MOUTH_EXTRA_COUNT

    fun pointsFromFlat(flat: List<Double>): List<CallFacePoint> {
        val points = ArrayList<CallFacePoint>()
        var i = 0
        while (i + 1 < flat.size) {
            points.add(CallFacePoint(flat[i], flat[i + 1]))
            i += 2
        }
        return points
    }

    fun centroid(points: List<CallFacePoint>, indices: List<Int>): CallFacePoint {
        var x = 0.0
        var y = 0.0
        for (i in indices) {
            x += points[i].x
            y += points[i].y
        }
        return CallFacePoint(x / indices.size, y / indices.size)
    }

    fun faceFrame(points: List<CallFacePoint>): CallFaceFrame = faceFrame(centroid(points, leftEye), centroid(points, rightEye))

    internal fun faceFrame(left: CallFacePoint, right: CallFacePoint): CallFaceFrame {
        val dx = right.x - left.x
        val dy = right.y - left.y
        val span = CallFaceJS.or(CallFaceJS.hypot(dx, dy), 1.0)
        val ux = CallFacePoint(dx / span, dy / span)
        return CallFaceFrame(
            left = left,
            right = right,
            mid = CallFacePoint((left.x + right.x) / 2, (left.y + right.y) / 2),
            span = span,
            ux = ux,
            uy = CallFacePoint(-ux.y, ux.x),
            roll = StrictMath.atan2(dy, dx),
        )
    }

    internal fun toFrame(frame: CallFaceFrame, point: CallFacePoint): CallFacePoint {
        val x = point.x - frame.mid.x
        val y = point.y - frame.mid.y
        return CallFacePoint(x * frame.ux.x + y * frame.ux.y, x * frame.uy.x + y * frame.uy.y)
    }

    internal fun fromFrame(frame: CallFaceFrame, a: Double, b: Double): CallFacePoint =
        CallFacePoint(frame.mid.x + a * frame.ux.x + b * frame.uy.x, frame.mid.y + a * frame.ux.y + b * frame.uy.y)

    fun normalizeShape(points: List<CallFacePoint>): List<CallFacePoint> {
        val frame = faceFrame(points)
        return points.map { point ->
            val local = toFrame(frame, point)
            CallFacePoint(local.x / frame.span, local.y / frame.span)
        }
    }

    fun mouthGap(points: List<CallFacePoint>): Double {
        val frame = faceFrame(points)
        val top = toFrame(frame, points[62])
        val bottom = toFrame(frame, points[66])
        return (bottom.y - top.y) / frame.span
    }

    fun buildTemplate(shapes: List<List<CallFacePoint>>): List<CallFacePoint> {
        val normalized = shapes.map(::normalizeShape)
        val closed = normalized.filter { it[66].y - it[62].y < 0.05 }
        val mouthSource = if (closed.isEmpty()) normalized else closed
        val mean = ArrayList<CallFacePoint>()
        for (i in 0 until LANDMARK_COUNT) {
            val pool = if (i >= 48) mouthSource else normalized
            var x = 0.0
            var y = 0.0
            for (shape in pool) {
                x += shape[i].x
                y += shape[i].y
            }
            mean.add(CallFacePoint(x / pool.size, y / pool.size))
        }
        val symmetric = mean.mapIndexed { i, point ->
            val twin = mean[mirror[i]]
            CallFacePoint((point.x - twin.x) / 2, (point.y + twin.y) / 2)
        }.toMutableList()
        for ((upper, lower) in listOf(61 to 67, 62 to 66, 63 to 65)) {
            val y = (symmetric[upper].y + symmetric[lower].y) / 2
            symmetric[upper] = symmetric[upper].copy(y = y)
            symmetric[lower] = symmetric[lower].copy(y = y)
        }
        return symmetric.mapIndexed { i, point -> CallFacePoint(point.x, point.y, depthTable[i]) }
    }

    fun fitTemplate(template: List<CallFacePoint>, anchors: CallFaceAnchors): List<CallFacePoint> {
        val yaw = clampValue(if (CallFaceJS.truthy(anchors.yaw)) anchors.yaw!! else 0.0, -0.7, 0.7)
        val cosine = StrictMath.cos(yaw)
        val sine = StrictMath.sin(yaw)
        val turned = template.map { CallFacePoint(it.x * cosine + it.depth * sine, it.y) }
        val frame = faceFrame(turned)
        val eyeL = anchors.eyeL
        val eyeR = anchors.eyeR
        val dx = eyeR.x - eyeL.x
        val dy = eyeR.y - eyeL.y
        val span = CallFaceJS.or(CallFaceJS.hypot(dx, dy), 1.0)
        val scale = span / frame.span
        val angle = StrictMath.atan2(dy, dx) - frame.roll
        val c = StrictMath.cos(angle) * scale
        val s = StrictMath.sin(angle) * scale
        val mid = CallFacePoint((eyeL.x + eyeR.x) / 2, (eyeL.y + eyeR.y) / 2)
        val placed = turned.map { point ->
            val px = point.x - frame.mid.x
            val py = point.y - frame.mid.y
            CallFacePoint(mid.x + px * c - py * s, mid.y + px * s + py * c)
        }
        val live = faceFrame(placed)
        val lx = DoubleArray(placed.size)
        val ly = DoubleArray(placed.size)
        placed.forEachIndexed { k, point ->
            val local = toFrame(live, point)
            lx[k] = local.x
            ly[k] = local.y
        }
        val mouthAtX = (lx[48] + lx[54]) / 2
        val mouthAtY = (ly[48] + ly[54]) / 2
        val mouth = anchors.mouth
        if (mouth != null) {
            val target = toFrame(live, mouth)
            val stretch = clampValue(target.y / CallFaceJS.or(mouthAtY, 1.0), 0.7, 1.35)
            val shift = clampValue(target.x - mouthAtX, -0.35 * live.span, 0.35 * live.span)
            for (k in lx.indices) {
                if (ly[k] > 0) {
                    lx[k] += shift * clampValue(ly[k] / mouthAtY, 0.0, 1.4)
                    ly[k] *= stretch
                } else {
                    ly[k] *= 1 + (stretch - 1) * 0.5
                }
            }
        }
        val centerX = (lx[48] + lx[54]) / 2
        if (CallFaceJS.truthy(anchors.mouthWidth)) {
            val width = CallFaceJS.or(CallFaceJS.hypot(lx[54] - lx[48], ly[54] - ly[48]), 1.0)
            val ratio = StrictMath.pow(clampValue(anchors.mouthWidth!! / width, 0.75, 1.4), 0.8)
            for (i in 48 until 68) lx[i] = centerX + (lx[i] - centerX) * ratio
        }
        val drop = clampValue(if (CallFaceJS.truthy(anchors.open)) anchors.open!! else 0.0, 0.0, 1.0) * MAX_OPEN * live.span
        if (drop > 0) {
            for ((i, weight) in openProfile) ly[i] += drop * weight
            for ((i, weight) in liftProfile) ly[i] -= drop * 0.08 * weight
            for ((i, weight) in chinDrop) ly[i] += drop * 0.85 * weight
        }
        return lx.indices.map { fromFrame(live, lx[it], ly[it]) }
    }

    private class Circle(val x: Double, val y: Double, val r: Double)
    private class Tri(val v: IntArray, val c: Circle)

    fun delaunay(points: List<CallFacePoint>): List<IntArray> {
        var minX = Double.POSITIVE_INFINITY
        var minY = Double.POSITIVE_INFINITY
        var maxX = Double.NEGATIVE_INFINITY
        var maxY = Double.NEGATIVE_INFINITY
        for (point in points) {
            minX = CallFaceJS.min(minX, point.x)
            minY = CallFaceJS.min(minY, point.y)
            maxX = CallFaceJS.max(maxX, point.x)
            maxY = CallFaceJS.max(maxY, point.y)
        }
        val size = CallFaceJS.or(CallFaceJS.max(maxX - minX, maxY - minY), 1.0)
        val midX = (minX + maxX) / 2
        val midY = (minY + maxY) / 2
        val all = points + listOf(
            CallFacePoint(midX - 20 * size, midY - size),
            CallFacePoint(midX, midY + 20 * size),
            CallFacePoint(midX + 20 * size, midY - size),
        )
        val n = points.size
        fun circle(a: Int, b: Int, c: Int): Circle {
            val ax = all[a].x
            val ay = all[a].y
            val bx = all[b].x
            val by = all[b].y
            val cx = all[c].x
            val cy = all[c].y
            val d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
            if (Math.abs(d) < 1e-12) return Circle(0.0, 0.0, Double.POSITIVE_INFINITY)
            val a2 = ax * ax + ay * ay
            val b2 = bx * bx + by * by
            val c2 = cx * cx + cy * cy
            val x = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d
            val y = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d
            return Circle(x, y, (x - ax) * (x - ax) + (y - ay) * (y - ay))
        }
        var triangles = listOf(Tri(intArrayOf(n, n + 1, n + 2), circle(n, n + 1, n + 2)))
        for (i in 0 until n) {
            val px = all[i].x
            val py = all[i].y
            val bad = ArrayList<Tri>()
            val keep = ArrayList<Tri>()
            for (t in triangles) {
                if ((px - t.c.x) * (px - t.c.x) + (py - t.c.y) * (py - t.c.y) < t.c.r) bad.add(t) else keep.add(t)
            }
            val edges = LinkedHashMap<Long, IntArray?>()
            for (t in bad) {
                for ((a, b) in listOf(t.v[0] to t.v[1], t.v[1] to t.v[2], t.v[2] to t.v[0])) {
                    val key = if (a < b) (a.toLong() shl 32) or b.toLong() else (b.toLong() shl 32) or a.toLong()
                    if (edges.containsKey(key)) edges[key] = null else edges[key] = intArrayOf(a, b)
                }
            }
            for (edge in edges.values) {
                if (edge != null) keep.add(Tri(intArrayOf(edge[0], edge[1], i), circle(edge[0], edge[1], i)))
            }
            triangles = keep
        }
        return triangles.filter { t -> t.v.all { it < n } }.map { it.v }
    }

    private fun catmullWeights(t: Double): DoubleArray {
        val t2 = t * t
        val t3 = t2 * t
        return doubleArrayOf(0.5 * (-t + 2 * t2 - t3), 0.5 * (2 - 5 * t2 + 3 * t3), 0.5 * (t + 4 * t2 - 3 * t3), 0.5 * (t3 - t2))
    }

    private fun lipSamples(control: List<Int>): List<Weights> {
        val last = control.size - 1
        fun at(j: Int): List<Pair<Int, Double>> {
            if (j < 0) return listOf(control[0] to 2.0, control[1] to -1.0)
            if (j > last) return listOf(control[last] to 2.0, control[last - 1] to -1.0)
            return listOf(control[j] to 1.0)
        }
        val samples = ArrayList<Weights>()
        for (j in 0 until last) {
            samples.add(Weights.of(control[j] to 1.0))
            for (s in 1 until LIP_STEPS) {
                val w = catmullWeights(s.toDouble() / LIP_STEPS)
                val weights = Weights()
                listOf(j - 1, j, j + 1, j + 2).forEachIndexed { n, k ->
                    for ((index, scale) in at(k)) weights.add(index, w[n] * scale)
                }
                samples.add(weights)
            }
        }
        samples.add(Weights.of(control[last] to 1.0))
        return samples
    }

    private fun blendWeights(a: Weights, b: Weights, amount: Double): Weights {
        val weights = Weights()
        for (k in 0 until a.size) weights.add(a.index[k], a.weight[k] * (1 - amount))
        for (k in 0 until b.size) weights.add(b.index[k], b.weight[k] * amount)
        return weights
    }

    private fun buildMouth(): Mouth {
        val extras = ArrayList<Weights>()
        fun reference(weights: Weights): MouthRef {
            if (weights.size == 1 && weights.weight[0] == 1.0) return MouthRef(weights.index[0], null, weights)
            extras.add(weights)
            return MouthRef(null, extras.size - 1, weights)
        }
        val upper = lipSamples(upperInner).map(::reference)
        val lower = lipSamples(lowerInner).map(::reference)
        val contourCount = extras.size
        val last = upper.size - 1
        fun inner(from: List<MouthRef>, to: List<MouthRef>, amount: Double): List<MouthRef> =
            from.mapIndexed { k, ref -> if (k == 0 || k == last) ref else reference(blendWeights(ref.weights, to[k].weights, amount)) }
        val innerUpper = inner(upper, lower, MOUTH_FEATHER)
        val innerLower = inner(lower, upper, MOUTH_FEATHER)
        val deepUpper = inner(upper, lower, MOUTH_DEEP)
        val deepLower = inner(lower, upper, MOUTH_DEEP)
        return Mouth(extras, upper, lower, innerUpper, innerLower, deepUpper, deepLower, contourCount)
    }

    private fun combine(points: List<CallFacePoint>, weights: Weights): CallFacePoint {
        var dims = Int.MAX_VALUE
        for (i in weights.index) dims = minOf(dims, points[i].dims)
        val out = DoubleArray(dims)
        for (k in 0 until weights.size) {
            val values = points[weights.index[k]].values
            val w = weights.weight[k]
            for (d in 0 until dims) out[d] += w * values[d]
        }
        return CallFacePoint.of(out)
    }

    fun mouthExtras(points: List<CallFacePoint>): List<CallFacePoint> = mouth.extras.map { combine(points, it) }

    fun cheekExtras(points: List<CallFacePoint>, lift: Boolean = false): List<CallFacePoint> = cheeks.map { (weights, bulge) ->
        val point = combine(points, weights)
        if (lift && point.z != null) point.copy(z = point.z + bulge) else point
    }

    fun mouthContour(points: List<CallFacePoint>): List<CallFacePoint> {
        fun pick(ref: MouthRef): CallFacePoint = ref.landmark?.let { points[it] } ?: combine(points, ref.weights)
        return mouth.upper.map(::pick) + mouth.lower.subList(1, mouth.lower.size - 1).reversed().map(::pick)
    }

    fun insidePolygon(polygon: List<CallFacePoint>, point: CallFacePoint): Boolean {
        var inside = false
        var j = polygon.size - 1
        for (i in polygon.indices) {
            val xi = polygon[i].x
            val yi = polygon[i].y
            val xj = polygon[j].x
            val yj = polygon[j].y
            if ((yi > point.y) != (yj > point.y) && point.x < ((xj - xi) * (point.y - yi)) / (yj - yi) + xi) inside = !inside
            j = i
        }
        return inside
    }

    fun headLayout(points: List<CallFacePoint>, ring: List<CallFacePoint>, forehead: Double = FOREHEAD_LIFT): List<CallFacePoint> {
        val frame = faceFrame(points)
        val lift = forehead * frame.span
        val out = points.map { it.flat }.toMutableList()
        for (i in 17..26) out.add(CallFacePoint(points[i].x - frame.uy.x * lift, points[i].y - frame.uy.y * lift))
        for (point in ring) out.add(point.flat)
        for ((anchor, point) in sideAnchors(out, ring.size)) {
            out.add(CallFacePoint(out[anchor].x + (point.x - out[anchor].x) * SIDE_REACH, out[anchor].y + (point.y - out[anchor].y) * SIDE_REACH))
        }
        return out + cheekExtras(out) + mouthExtras(out)
    }

    private fun sideAnchors(layout: List<CallFacePoint>, count: Int): List<Pair<Int, CallFacePoint>> {
        val edge = jaw + (LANDMARK_COUNT until LANDMARK_COUNT + 10).toList()
        return layout.subList(RING_START, RING_START + count).map { point ->
            var best = edge[0]
            var near = Double.POSITIVE_INFINITY
            for (i in edge) {
                val dx = layout[i].x - point.x
                val dy = layout[i].y - point.y
                val d = dx * dx + dy * dy
                if (d < near) {
                    near = d
                    best = i
                }
            }
            best to point
        }
    }

    fun ringCount(length: Int): Int = (length - EXTRA_COUNT - RING_START) / 2

    private fun vertexDepth(i: Int, count: Int, anchors: List<Pair<Int, CallFacePoint>>): Double {
        if (i < LANDMARK_COUNT) return depthTable[i]
        if (i < RING_START) return depthTable[i - LANDMARK_COUNT + 17] * FOREHEAD_CURVE + FOREHEAD_BACK
        if (i < RING_START + count) return RING_DEPTH
        return vertexDepth(anchors[i - RING_START - count].first, count, anchors) - SIDE_BACK
    }

    fun headModel(points: List<CallFacePoint>, ring: List<CallFacePoint>): List<CallFacePoint> {
        val frame = faceFrame(points)
        val layout = headLayout(points, ring)
        val anchors = sideAnchors(layout, ring.size)
        val base = layout.subList(0, layout.size - EXTRA_COUNT).mapIndexed { i, point ->
            val local = toFrame(frame, point)
            CallFacePoint(local.x / frame.span, local.y / frame.span, vertexDepth(i, ring.size, anchors))
        }
        return base + cheekExtras(base, lift = true) + mouthExtras(base)
    }

    private fun triangleArea(points: List<CallFacePoint>, v: IntArray): Double {
        val a = points[v[0]]
        val b = points[v[1]]
        val c = points[v[2]]
        return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)
    }

    fun buildHeadMesh(model: List<CallFacePoint>, template: List<CallFacePoint>): CallFaceHeadMesh {
        val open = fitTemplate(template, CallFaceAnchors(eyeL = CallFacePoint(-0.5, 0.0), eyeR = CallFacePoint(0.5, 0.0), open = 0.6))
        val start = model.size - EXTRA_COUNT
        val mouthStart = start + CHEEK_COUNT
        val base = model.subList(0, start).mapIndexed { i, point -> if (i < LANDMARK_COUNT) open[i] else point.flat }
        val layout = base + cheekExtras(base) + mouthExtras(open)
        fun vertex(ref: MouthRef): Int = ref.landmark ?: (mouthStart + ref.extra!!)
        val contour = mouth.upper.map(::vertex) + mouth.lower.subList(1, mouth.lower.size - 1).reversed().map(::vertex)
        val onContour = contour.toSet()
        val polygon = contour.map { layout[it] }
        val triangles = delaunay(layout.subList(0, mouthStart + mouth.contourCount))
            .filter { v -> !(v.all { it in onContour } && insidePolygon(polygon, centroid(layout, v.toList()))) }
            .filter { Math.abs(triangleArea(model, it)) > 1e-9 }
            .map { v -> CallFaceTriangle(v, doubleArrayOf(0.0, 0.0, 0.0), false, !v.any { (it >= 48 && it < LANDMARK_COUNT) || it >= mouthStart }) }
            .toMutableList()
        val corners = setOf(upperInner[0], upperInner[upperInner.size - 1])
        fun add(v: IntArray, hole: DoubleArray) {
            if (v.toSet().size < 3) return
            val holes = DoubleArray(3) { k -> if (v[k] in corners) CallFaceJS.min(hole[k], 0.5) else hole[k] }
            triangles.add(CallFaceTriangle(v, holes, true, false))
        }
        fun strip(a: List<MouthRef>, b: List<MouthRef>, ha: Double, hb: Double) {
            var k = 0
            while (k + 1 < a.size) {
                val p = vertex(a[k])
                val q = vertex(a[k + 1])
                val r = vertex(b[k + 1])
                val s = vertex(b[k])
                add(intArrayOf(p, q, r), doubleArrayOf(ha, ha, hb))
                add(intArrayOf(p, r, s), doubleArrayOf(ha, hb, hb))
                k += 1
            }
        }
        strip(mouth.upper, mouth.innerUpper, 0.0, 1.0)
        strip(mouth.lower, mouth.innerLower, 0.0, 1.0)
        strip(mouth.innerUpper, mouth.deepUpper, 1.0, 2.0)
        strip(mouth.innerLower, mouth.deepLower, 1.0, 2.0)
        strip(mouth.deepUpper, mouth.deepLower, 2.0, 2.0)
        return CallFaceHeadMesh(triangles, layout.size, start, mouthStart)
    }

    fun hasDepth(live: List<CallFacePoint>): Boolean {
        var low = Double.POSITIVE_INFINITY
        var high = Double.NEGATIVE_INFINITY
        for (i in 0 until LANDMARK_COUNT) {
            if (poseWeights[i] == 0.0) continue
            if (i >= live.size) return false
            val z = live[i].z
            if (z == null || !z.isFinite()) return false
            low = CallFaceJS.min(low, z)
            high = CallFaceJS.max(high, z)
        }
        val span = CallFaceJS.or(CallFaceJS.hypot(live[45].x - live[36].x, live[45].y - live[36].y), 1.0)
        return high - low > DEPTH_SPREAD * span
    }

    private fun dominantEigenvector(matrix: Array<DoubleArray>): DoubleArray {
        val a = Array(4) { matrix[it].copyOf() }
        val v = Array(4) { i -> DoubleArray(4) { j -> if (i == j) 1.0 else 0.0 } }
        for (sweep in 0 until 24) {
            var off = 0.0
            for (p in 0 until 4) for (q in p + 1 until 4) off += a[p][q] * a[p][q]
            if (off < 1e-18) break
            for (p in 0 until 4) {
                for (q in p + 1 until 4) {
                    if (Math.abs(a[p][q]) < 1e-20) continue
                    val theta = (a[q][q] - a[p][p]) / (2 * a[p][q])
                    val t = CallFaceJS.sign(CallFaceJS.or(theta, 1.0)) / (Math.abs(theta) + StrictMath.sqrt(theta * theta + 1))
                    val c = 1 / StrictMath.sqrt(t * t + 1)
                    val s = t * c
                    for (k in 0 until 4) {
                        val akp = a[k][p]
                        val akq = a[k][q]
                        a[k][p] = c * akp - s * akq
                        a[k][q] = s * akp + c * akq
                    }
                    for (k in 0 until 4) {
                        val apk = a[p][k]
                        val aqk = a[q][k]
                        a[p][k] = c * apk - s * aqk
                        a[q][k] = s * apk + c * aqk
                    }
                    for (k in 0 until 4) {
                        val vkp = v[k][p]
                        val vkq = v[k][q]
                        v[k][p] = c * vkp - s * vkq
                        v[k][q] = s * vkp + c * vkq
                    }
                }
            }
        }
        var best = 0
        for (i in 1 until 4) if (a[i][i] > a[best][best]) best = i
        return doubleArrayOf(v[0][best], v[1][best], v[2][best], v[3][best])
    }

    private fun component(point: CallFacePoint, k: Int): Double = if (k == 0) point.x else if (k == 1) point.y else point.depth

    fun solvePoseWithDepth(model: List<CallFacePoint>, live: List<CallFacePoint>, weights: DoubleArray = poseWeights): CallFacePose? {
        var total = 0.0
        val mm = DoubleArray(3)
        val lm = DoubleArray(3)
        for (i in 0 until LANDMARK_COUNT) {
            val w = weights[i]
            if (!CallFaceJS.truthy(w)) continue
            total += w
            for (k in 0 until 3) {
                mm[k] += w * component(model[i], k)
                lm[k] += w * component(live[i], k)
            }
        }
        for (k in 0 until 3) {
            mm[k] /= total
            lm[k] /= total
        }
        val S = Array(3) { DoubleArray(3) }
        for (i in 0 until LANDMARK_COUNT) {
            val w = weights[i]
            if (!CallFaceJS.truthy(w)) continue
            for (a in 0 until 3) for (b in 0 until 3) S[a][b] += w * (component(model[i], a) - mm[a]) * (component(live[i], b) - lm[b])
        }
        val sxx = S[0][0]
        val sxy = S[0][1]
        val sxz = S[0][2]
        val syx = S[1][0]
        val syy = S[1][1]
        val syz = S[1][2]
        val szx = S[2][0]
        val szy = S[2][1]
        val szz = S[2][2]
        val q = dominantEigenvector(arrayOf(
            doubleArrayOf(sxx + syy + szz, syz - szy, szx - sxz, sxy - syx),
            doubleArrayOf(syz - szy, sxx - syy - szz, sxy + syx, szx + sxz),
            doubleArrayOf(szx - sxz, sxy + syx, -sxx + syy - szz, syz + szy),
            doubleArrayOf(sxy - syx, szx + sxz, syz + szy, -sxx - syy + szz),
        ))
        val qw = q[0]
        val qx = q[1]
        val qy = q[2]
        val qz = q[3]
        val r1 = doubleArrayOf(qw * qw + qx * qx - qy * qy - qz * qz, 2 * (qx * qy - qw * qz), 2 * (qx * qz + qw * qy))
        val r2 = doubleArrayOf(2 * (qx * qy + qw * qz), qw * qw - qx * qx + qy * qy - qz * qz, 2 * (qy * qz - qw * qx))
        val r3 = doubleArrayOf(2 * (qx * qz - qw * qy), 2 * (qy * qz + qw * qx), qw * qw - qx * qx - qy * qy + qz * qz)
        var num = 0.0
        var den = 0.0
        for (i in 0 until LANDMARK_COUNT) {
            val w = weights[i]
            if (!CallFaceJS.truthy(w)) continue
            val m = doubleArrayOf(model[i].x - mm[0], model[i].y - mm[1], model[i].depth - mm[2])
            val px = dot3(r1, m)
            val py = dot3(r2, m)
            num += w * ((live[i].x - lm[0]) * px + (live[i].y - lm[1]) * py)
            den += w * (px * px + py * py)
        }
        if (!(den > 0) || !(num > 0)) return null
        val scale = num / den
        return CallFacePose(
            scale = scale,
            rows = listOf(r1, r2, r3),
            t = doubleArrayOf(lm[0] - scale * dot3(r1, mm), lm[1] - scale * dot3(r2, mm)),
            roll = StrictMath.atan2(r2[0], r1[0]),
            yaw = StrictMath.asin(clampValue(-r3[0], -1.0, 1.0)),
            pitch = StrictMath.asin(clampValue(r3[1], -1.0, 1.0)),
            depth = true,
        )
    }

    fun solvePose(model: List<CallFacePoint>, live: List<CallFacePoint>, weights: DoubleArray = poseWeights): CallFacePose {
        if (hasDepth(live)) solvePoseWithDepth(model, live, weights)?.let { return it }
        var total = 0.0
        val mm = DoubleArray(3)
        val lm = DoubleArray(2)
        for (i in 0 until LANDMARK_COUNT) {
            val w = weights[i]
            if (!CallFaceJS.truthy(w)) continue
            total += w
            for (k in 0 until 3) mm[k] += w * component(model[i], k)
            lm[0] += w * live[i].x
            lm[1] += w * live[i].y
        }
        for (k in 0 until 3) mm[k] /= total
        lm[0] /= total
        lm[1] /= total
        val mtm = Array(3) { DoubleArray(3) }
        val ltm = Array(2) { DoubleArray(3) }
        for (i in 0 until LANDMARK_COUNT) {
            val w = weights[i]
            if (!CallFaceJS.truthy(w)) continue
            val m = doubleArrayOf(model[i].x - mm[0], model[i].y - mm[1], model[i].depth - mm[2])
            val l = doubleArrayOf(live[i].x - lm[0], live[i].y - lm[1])
            for (a in 0 until 3) {
                for (b in 0 until 3) mtm[a][b] += w * m[a] * m[b]
                ltm[0][a] += w * l[0] * m[a]
                ltm[1][a] += w * l[1] * m[a]
            }
        }
        for (a in 0 until 3) mtm[a][a] += 1e-4
        val inverse = invert3(mtm)
        val rows = ltm.map { row -> DoubleArray(3) { b -> row[0] * inverse[0][b] + row[1] * inverse[1][b] + row[2] * inverse[2][b] } }
        val s1 = CallFaceJS.or(CallFaceJS.hypot(rows[0][0], rows[0][1], rows[0][2]), 1.0)
        val s2 = CallFaceJS.or(CallFaceJS.hypot(rows[1][0], rows[1][1], rows[1][2]), 1.0)
        val a = DoubleArray(3) { rows[0][it] / s1 }
        val b = DoubleArray(3) { rows[1][it] / s2 }
        val c = normalize3(DoubleArray(3) { a[it] + b[it] })
        val d = normalize3(DoubleArray(3) { a[it] - b[it] })
        val sqrt2 = StrictMath.sqrt(2.0)
        val r1 = DoubleArray(3) { (c[it] + d[it]) / sqrt2 }
        val r2 = DoubleArray(3) { (c[it] - d[it]) / sqrt2 }
        val scale = (s1 + s2) / 2
        if (noseDisagrees(model, live, r1, r2, scale)) {
            r1[2] = -r1[2]
            r2[2] = -r2[2]
        }
        val r3 = doubleArrayOf(r1[1] * r2[2] - r1[2] * r2[1], r1[2] * r2[0] - r1[0] * r2[2], r1[0] * r2[1] - r1[1] * r2[0])
        val t = doubleArrayOf(lm[0] - scale * dot3(r1, mm), lm[1] - scale * dot3(r2, mm))
        return CallFacePose(
            scale = scale,
            rows = listOf(r1, r2, r3),
            t = t,
            roll = StrictMath.atan2(r2[0], r1[0]),
            yaw = StrictMath.asin(clampValue(-r3[0], -1.0, 1.0)),
            pitch = StrictMath.asin(clampValue(r3[1], -1.0, 1.0)),
            depth = false,
        )
    }

    fun noseTurn(points: List<CallFacePoint>): Double {
        val frame = faceFrame(points)
        val tip = toFrame(frame, points[30])
        val bridge = toFrame(frame, points[27])
        return (tip.x - bridge.x) / frame.span
    }

    private fun noseDisagrees(model: List<CallFacePoint>, live: List<CallFacePoint>, r1: DoubleArray, r2: DoubleArray, scale: Double): Boolean {
        val seen = noseTurn(live) - (model[30].x - model[27].x)
        if (Math.abs(seen) < NOSE_TRUST) return false
        val turn = r1[2] * (model[30].depth - model[27].depth)
        val a = CallFaceJS.sign(turn)
        val b = CallFaceJS.sign(seen)
        return !(a == b) && Math.abs(turn) > NOSE_TRUST * 0.5
    }

    fun projectPose(pose: CallFacePose, point: CallFacePoint): CallFacePoint {
        val p = doubleArrayOf(point.x, point.y, point.depth)
        return CallFacePoint(pose.t[0] + pose.scale * dot3(pose.rows[0], p), pose.t[1] + pose.scale * dot3(pose.rows[1], p))
    }

    fun projectOutline(pose: CallFacePose, point: CallFacePoint): CallFacePoint {
        val center = projectPose(pose, headCenter)
        val c = StrictMath.cos(pose.roll)
        val s = StrictMath.sin(pose.roll)
        val dx = (point.x - headCenter.x) * pose.scale
        val dy = (point.y - headCenter.y) * pose.scale
        return CallFacePoint(center.x + dx * c - dy * s, center.y + dx * s + dy * c)
    }

    fun rigidNose(model: List<CallFacePoint>, live: List<CallFacePoint>, pose: CallFacePose, out: MutableList<CallFacePoint>) {
        val rigid = nose.map { projectPose(pose, model[it]) }
        var sx = 0.0
        var sy = 0.0
        nose.forEachIndexed { k, i ->
            sx += live[i].x - rigid[k].x
            sy += live[i].y - rigid[k].y
        }
        sx /= nose.size
        sy /= nose.size
        nose.forEachIndexed { k, i ->
            val x = rigid[k].x + sx
            val y = rigid[k].y + sy
            out[i] = CallFacePoint(live[i].x + (x - live[i].x) * NOSE_RIGID, live[i].y + (y - live[i].y) * NOSE_RIGID)
        }
    }

    fun jawTrust(model: List<CallFacePoint>, pose: CallFacePose): DoubleArray {
        val r3 = pose.rows[2]
        return DoubleArray(jaw.size) { k ->
            val i = jaw[k]
            val x = model[i].x
            val y = model[i].y
            val facing = dot3(r3, normalize3(doubleArrayOf(x, CallFaceJS.max(0.0, y - 1) * 0.8, JAW_NORMAL_Z)))
            smoothstepValue(JAW_FACING_LOW, JAW_FACING_HIGH, facing)
        }
    }

    private class Source(val i: Int, val dx: Double, val dy: Double, val trust: Double)

    private fun residualField(model: List<CallFacePoint>, sources: List<Source>, at: CallFacePoint): Pair<Double, Double> {
        var wx = 0.0
        var wy = 0.0
        var weight = RESIDUAL_ANCHOR
        for (source in sources) {
            if (!CallFaceJS.truthy(source.trust)) continue
            val ex = at.x - model[source.i].x
            val ey = at.y - model[source.i].y
            val d2 = ex * ex + ey * ey
            val w = source.trust * StrictMath.exp(-d2 / (2 * RESIDUAL_SIGMA * RESIDUAL_SIGMA))
            wx += w * source.dx
            wy += w * source.dy
            weight += w
        }
        return (wx / weight) to (wy / weight)
    }

    private class MouthAxes(val mid: CallFacePoint, val ux: CallFacePoint, val half: Double) {
        fun local(p: CallFacePoint): CallFacePoint =
            CallFacePoint(((p.x - mid.x) * ux.x + (p.y - mid.y) * ux.y) / half, ((p.x - mid.x) * -ux.y + (p.y - mid.y) * ux.x) / half)

        fun place(p: CallFacePoint): CallFacePoint =
            CallFacePoint(mid.x + (p.x * ux.x - p.y * ux.y) * half, mid.y + (p.x * ux.y + p.y * ux.x) * half)
    }

    private fun mouthAxes(points: List<CallFacePoint>): MouthAxes? {
        val a = points[48]
        val b = points[54]
        val half = CallFaceJS.hypot(b.x - a.x, b.y - a.y) / 2
        if (!(half > 1e-6)) return null
        val ux = CallFacePoint((b.x - a.x) / (2 * half), (b.y - a.y) / (2 * half))
        return MouthAxes(CallFacePoint((a.x + b.x) / 2, (a.y + b.y) / 2), ux, half)
    }

    private fun lipCurve(local: Array<CallFacePoint?>, indices: List<Int>): List<CallFacePoint> =
        CallFaceJS.sorted(indices.map { local[it]!! }) { p, q -> p.x - q.x }

    private fun curveAt(curve: List<CallFacePoint>, s: Double): Double {
        if (s <= curve[0].x) return curve[0].y
        for (k in 1 until curve.size) {
            val s1 = curve[k].x
            val t1 = curve[k].y
            if (s <= s1) {
                val s0 = curve[k - 1].x
                val t0 = curve[k - 1].y
                return if (s1 - s0 > 1e-9) t0 + ((t1 - t0) * (s - s0)) / (s1 - s0) else t1
            }
        }
        return curve[curve.size - 1].y
    }

    fun regularMouth(reference: List<CallFacePoint>, points: List<CallFacePoint>): List<CallFacePoint> {
        val modelAxes = mouthAxes(reference) ?: return points
        val liveAxes = mouthAxes(points) ?: return points
        val ref = arrayOfNulls<CallFacePoint>(LANDMARK_COUNT)
        val seen = arrayOfNulls<CallFacePoint>(LANDMARK_COUNT)
        for (i in 48 until 68) {
            ref[i] = modelAxes.local(reference[i])
            seen[i] = liveAxes.local(points[i])
        }
        val refUpper = lipCurve(ref, upperOuter)
        val refLower = lipCurve(ref, lowerOuter)
        val refInnerUpper = lipCurve(ref, upperInner)
        val refInnerLower = lipCurve(ref, lowerInner)
        val upper = lipCurve(seen, upperOuter)
        val lower = lipCurve(seen, lowerOuter)
        val innerUpper = lipCurve(seen, upperInner)
        val innerLower = lipCurve(seen, lowerInner)
        val placed = arrayOfNulls<CallFacePoint>(LANDMARK_COUNT)
        for (i in listOf(49, 50, 51, 52, 53)) placed[i] = CallFacePoint(ref[i]!!.x, curveAt(upper, ref[i]!!.x))
        for (i in listOf(55, 56, 57, 58, 59)) placed[i] = CallFacePoint(ref[i]!!.x, curveAt(lower, ref[i]!!.x))
        placed[48] = seen[48]
        placed[54] = seen[54]
        val top = lipCurve(placed, upperOuter)
        val bottom = lipCurve(placed, lowerOuter)
        val reach = CallFaceJS.max(Math.abs(ref[60]!!.x), Math.abs(ref[64]!!.x), 1e-3)
        val centerGap = CallFaceJS.max(0.0, curveAt(innerLower, 0.0) - curveAt(innerUpper, 0.0))
        fun inner(s: Double): Pair<Double, Double> {
            val roof = curveAt(top, s) + LIP_KEEP * CallFaceJS.max(0.0, curveAt(refInnerUpper, s) - curveAt(refUpper, s))
            val floor = curveAt(bottom, s) - LIP_KEEP * CallFaceJS.max(0.0, curveAt(refLower, s) - curveAt(refInnerLower, s))
            val seenUpper = curveAt(innerUpper, s)
            val seenLower = curveAt(innerLower, s)
            val a = CallFaceJS.max(seenUpper, roof)
            val b = CallFaceJS.min(seenLower, floor)
            if (b <= a) {
                val seam = clampValue((seenUpper + seenLower) / 2, CallFaceJS.min(roof, floor), CallFaceJS.max(roof, floor))
                return seam to seam
            }
            val middle = (a + b) / 2
            val ratio = s / reach
            val room = CallFaceJS.min(b - a, centerGap * StrictMath.pow(CallFaceJS.max(0.0, 1 - ratio * ratio), CORNER_TAPER))
            return (middle - room / 2) to (middle + room / 2)
        }
        for ((up, down) in listOf(61 to 67, 62 to 66, 63 to 65)) {
            val s = (ref[up]!!.x + ref[down]!!.x) / 2
            val (a, b) = inner(s)
            placed[up] = CallFacePoint(s, a)
            placed[down] = CallFacePoint(s, b)
        }
        for (i in listOf(60, 64)) {
            val s = ref[i]!!.x
            val a = curveAt(top, s)
            val b = curveAt(bottom, s)
            placed[i] = CallFacePoint(s, clampValue(seen[i]!!.y, CallFaceJS.min(a, b), CallFaceJS.max(a, b)))
        }
        val out = points.toMutableList()
        for (i in 48 until 68) out[i] = liveAxes.place(placed[i]!!)
        return out
    }

    fun liveHead(model: List<CallFacePoint>, live: List<CallFacePoint>): CallFaceLiveHead {
        val first = solvePose(model, live)
        val trust = jawTrust(model, first)
        val pose = solvePose(model, live, DoubleArray(poseWeights.size) { i -> if (i < jaw.size) poseWeights[i] * trust[i] else poseWeights[i] })
        val settled = jawTrust(model, pose)
        val start = model.size - EXTRA_COUNT
        val outlineEnd = RING_START + ringCount(model.size)
        val projected = model.mapIndexed { v, point -> if (v >= RING_START && v < outlineEnd) projectOutline(pose, point) else projectPose(pose, point) }
        fun source(i: Int, trust: Double): Source = Source(i, live[i].x - projected[i].x, live[i].y - projected[i].y, trust)
        val jawSources = jaw.map { source(it, settled[it] * JAW_FOLLOW) }
        val field = fieldSources.map { source(it.first, it.second) } + jawSources
        val cheekField = cheekSources.map { source(it.first, it.second) } + jawSources
        val out = live.subList(0, LANDMARK_COUNT).map { it.flat }.toMutableList()
        for (i in jaw) {
            val follow = settled[i] * JAW_FOLLOW
            val (fx, fy) = residualField(model, cheekField, model[i])
            out[i] = CallFacePoint(
                projected[i].x + follow * jawSources[i].dx + (1 - follow) * fx,
                projected[i].y + follow * jawSources[i].dy + (1 - follow) * fy,
            )
        }
        rigidNose(model, live, pose, out)
        val shaped = regularMouth(projected, out)
        for (i in 48 until LANDMARK_COUNT) out[i] = shaped[i]
        for (v in LANDMARK_COUNT until start + CHEEK_COUNT) {
            val (fx, fy) = residualField(model, if (v < start) field else cheekField, model[v])
            out.add(CallFacePoint(projected[v].x + fx, projected[v].y + fy))
        }
        return CallFaceLiveHead(out + mouthExtras(out), pose, settled)
    }

    fun vertexDepths(model: List<CallFacePoint>, pose: CallFacePose): DoubleArray {
        val r3 = pose.rows[2]
        val depths = DoubleArray(model.size) { dot3(r3, doubleArrayOf(model[it].x, model[it].y, model[it].depth)) }
        var jawDepth = Double.POSITIVE_INFINITY
        for (i in jaw) jawDepth = CallFaceJS.min(jawDepth, depths[i])
        var v = RING_START
        while (v < depths.size - EXTRA_COUNT) {
            depths[v] = CallFaceJS.min(depths[v], jawDepth - RING_GAP)
            v += 1
        }
        return depths
    }

    fun smoothstepValue(low: Double, high: Double, value: Double): Double {
        val t = clampValue((value - low) / (high - low), 0.0, 1.0)
        return t * t * (3 - 2 * t)
    }

    internal fun dot3(a: DoubleArray, b: DoubleArray): Double = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

    private fun normalize3(v: DoubleArray): DoubleArray {
        val length = CallFaceJS.or(CallFaceJS.hypot(v[0], v[1], v[2]), 1.0)
        return doubleArrayOf(v[0] / length, v[1] / length, v[2] / length)
    }

    private fun invert3(m: Array<DoubleArray>): Array<DoubleArray> {
        val a = m[0][0]
        val b = m[0][1]
        val c = m[0][2]
        val d = m[1][0]
        val e = m[1][1]
        val f = m[1][2]
        val g = m[2][0]
        val h = m[2][1]
        val i = m[2][2]
        val A = e * i - f * h
        val B = -(d * i - f * g)
        val C = d * h - e * g
        val det = CallFaceJS.or(a * A + b * B + c * C, 1e-9)
        return arrayOf(
            doubleArrayOf(A / det, -(b * i - c * h) / det, (b * f - c * e) / det),
            doubleArrayOf(B / det, (a * i - c * g) / det, -(a * f - c * d) / det),
            doubleArrayOf(C / det, -(a * h - b * g) / det, (a * e - b * d) / det),
        )
    }

    private val neededRegions = listOf("faceContour", "leftEyebrow", "rightEyebrow", "leftEye", "rightEye", "outerLips", "innerLips")

    private fun finiteZ(point: List<Double>): Boolean = point.size > 2 && point[2].isFinite()

    fun landmarksFromRegions(regions: CallFaceRegions?): List<CallFacePoint>? {
        if (regions == null) return null
        if (neededRegions.any { (regions[it]?.size ?: 0) < 3 }) return null
        val crest = if ((regions["noseCrest"]?.size ?: 0) >= 2) regions["noseCrest"] else regions["medianLine"]
        val noseRegion = regions["nose"]
        if (crest == null || crest.size < 2 || noseRegion == null || noseRegion.size < 3) return null
        val all = neededRegions.map { regions[it]!! } + listOf(crest, noseRegion)
        val dims = if (all.all { region -> region.all(::finiteZ) }) 3 else 2
        fun copy(point: List<Double>): List<Double> = point.take(dims)
        val out = arrayOfNulls<List<Double>>(LANDMARK_COUNT)
        var jawLine = regions["faceContour"]!!.map(::copy)
        if (jawLine[0][0] > jawLine[jawLine.size - 1][0]) jawLine = jawLine.reversed()
        resample(jawLine, 17).forEachIndexed { i, point -> out[i] = point }
        val brows = CallFaceJS.sorted(listOf(regions["leftEyebrow"]!!, regions["rightEyebrow"]!!)) { a, b -> meanX(a) - meanX(b) }
        browLine(brows[0].map(::copy)).forEachIndexed { i, point -> out[17 + i] = point }
        browLine(brows[1].map(::copy)).forEachIndexed { i, point -> out[22 + i] = point }
        val ridge = CallFaceJS.sorted(crest.map(::copy)) { a, b -> a[1] - b[1] }
        resample(ridge, 4).forEachIndexed { i, point -> out[27 + i] = point }
        val noseCopy = noseRegion.map(::copy)
        val middle = noseCopy.fold(0.0) { sum, p -> sum + p[1] } / noseCopy.size
        val base = CallFaceJS.sorted(noseCopy.filter { it[1] >= middle }) { a, b -> a[0] - b[0] }
        val noseBase = if (base.size >= 2) base else CallFaceJS.sorted(noseCopy) { a, b -> a[0] - b[0] }
        resample(noseBase, 5).forEachIndexed { i, point -> out[31 + i] = point }
        val eyes = CallFaceJS.sorted(listOf(regions["leftEye"]!!.map(::copy), regions["rightEye"]!!.map(::copy))) { a, b -> meanX(a) - meanX(b) }
        eyes.forEachIndexed { side, loop ->
            val (upper, lower) = splitLoop(loop, 4, 4)
            val at = if (side == 1) 42 else 36
            out[at] = upper[0]
            out[at + 1] = upper[1]
            out[at + 2] = upper[2]
            out[at + 3] = upper[3]
            out[at + 4] = lower[2]
            out[at + 5] = lower[1]
        }
        val (lipTop, lipBottom) = splitLoop(regions["outerLips"]!!.map(::copy), 7, 7)
        for (i in 0 until 7) out[48 + i] = lipTop[i]
        for (i in 1 until 6) out[60 - i] = lipBottom[i]
        val (innerTop, innerBottom) = splitLoop(regions["innerLips"]!!.map(::copy), 5, 5)
        for (i in 0 until 5) out[60 + i] = innerTop[i]
        for (i in 1 until 4) out[68 - i] = innerBottom[i]
        return out.map { CallFacePoint.of(it!!) }
    }

    fun shapeRegions(points: List<CallFacePoint>): CallFaceRegions {
        fun pick(indices: List<Int>): List<List<Double>> = indices.map { listOf(points[it].x, points[it].y) }
        return linkedMapOf(
            "faceContour" to pick(jaw),
            "leftEyebrow" to pick(listOf(17, 18, 19, 20, 21)),
            "rightEyebrow" to pick(listOf(22, 23, 24, 25, 26)),
            "noseCrest" to pick(listOf(27, 28, 29, 30)),
            "nose" to pick(listOf(31, 32, 33, 34, 35)),
            "leftEye" to pick(leftEye),
            "rightEye" to pick(rightEye),
            "outerLips" to pick(outerLips),
            "innerLips" to pick(innerLips),
        )
    }

    fun canonicalShape(points: List<CallFacePoint>): List<CallFacePoint> {
        val shaped = landmarksFromRegions(shapeRegions(points))?.toMutableList() ?: return points
        for (i in 27..35) shaped[i] = points[i].flat
        return shaped
    }

    private fun meanX(points: List<List<Double>>): Double = points.fold(0.0) { sum, p -> sum + p[0] } / points.size

    private fun browLine(points: List<List<Double>>): List<List<Double>> {
        val sorted = CallFaceJS.sorted(points) { a, b -> a[0] - b[0] }
        val low = sorted[0][0]
        val high = sorted[sorted.size - 1][0]
        val out = ArrayList<List<Double>>()
        for (i in 0 until 5) {
            val x = low + ((high - low) * i) / 4
            val near = CallFaceJS.sorted(sorted.map { Math.abs(it[0] - x) to it }) { a, b -> a.first - b.first }
                .take(minOf(2, sorted.size))
                .map { it.second }
            val point = mutableListOf(x, near.fold(0.0) { sum, p -> sum + p[1] } / near.size)
            if (near.all(::finiteZ)) point.add(near.fold(0.0) { sum, p -> sum + p[2] } / near.size)
            out.add(point)
        }
        return out
    }

    private fun splitLoop(points: List<List<Double>>, upperCount: Int, lowerCount: Int): Pair<List<List<Double>>, List<List<Double>>> {
        var left = 0
        var right = 0
        points.forEachIndexed { i, point ->
            if (point[0] < points[left][0]) left = i
            if (point[0] > points[right][0]) right = i
        }
        fun walk(from: Int, to: Int): List<List<Double>> {
            val path = arrayListOf(points[from])
            var i = from
            while (i != to) {
                i = (i + 1) % points.size
                path.add(points[i])
            }
            return path
        }
        val one = walk(left, right)
        val two = walk(right, left).reversed()
        fun meanY(path: List<List<Double>>): Double = path.fold(0.0) { sum, p -> sum + p[1] } / path.size
        val upper = if (meanY(one) <= meanY(two)) one else two
        val lower = if (meanY(one) <= meanY(two)) two else one
        return resample(upper, upperCount) to resample(lower, lowerCount)
    }

    private fun resample(points: List<List<Double>>, count: Int): List<List<Double>> {
        if (points.size == 1) return List(count) { points[0] }
        val lengths = arrayListOf(0.0)
        for (i in 1 until points.size) {
            lengths.add(lengths[i - 1] + CallFaceJS.hypot(points[i][0] - points[i - 1][0], points[i][1] - points[i - 1][1]))
        }
        val total = CallFaceJS.or(lengths[lengths.size - 1], 1.0)
        val out = ArrayList<List<Double>>()
        var segment = 1
        for (i in 0 until count) {
            val at = (total * i) / (count - 1)
            while (segment < points.size - 1 && lengths[segment] < at) segment += 1
            val span = CallFaceJS.or(lengths[segment] - lengths[segment - 1], 1.0)
            val t = clampValue((at - lengths[segment - 1]) / span, 0.0, 1.0)
            val a = points[segment - 1]
            val b = points[segment]
            out.add(a.mapIndexed { k, value -> value + ((if (k < b.size) b[k] else value) - value) * t })
        }
        return out
    }

    fun resamplePolyline(points: List<CallFacePoint>, count: Int): List<CallFacePoint> =
        resample(points.map { it.values.toList() }, count).map { CallFacePoint.of(it) }

    fun clampValue(value: Double, low: Double, high: Double): Double = CallFaceJS.min(high, CallFaceJS.max(low, value))
}
