package com.forkmesh.nativeapp.chat.face

data class CallFaceBox(val center: CallFacePoint, val ux: CallFacePoint, val uy: CallFacePoint, val half: CallFacePoint)

class CallFaceImage(val width: Int, val height: Int, val rgba: ByteArray) {
    fun channel(at: Int): Double = (rgba[at].toInt() and 0xff).toDouble()
}

data class CallFaceSkin(val luma: Double, val red: Double)

data class CallFaceEyeReading(val dark: Double, val edge: Double, val look: CallFacePoint)

data class CallFaceProbeCrop(val minX: Double, val minY: Double, val scale: Double, val width: Int, val height: Int) {
    fun local(point: CallFacePoint): CallFacePoint = CallFacePoint((point.x - minX) * scale, (point.y - minY) * scale)
}

data class CallFaceExpressionReading(
    val close: DoubleArray,
    val look: List<CallFacePoint>,
    val tongue: Double,
    val skinRed: Double,
    val skinLuma: Double,
)

class CallFaceBlinkInput(
    val edges: DoubleArray,
    val openness: DoubleArray? = null,
    val elapsed: Double,
    val time: Double,
    val moving: Boolean = false,
)

object CallFaceExpression {
    private const val PROBE_SPAN = 120.0
    private const val EYE_HALF_X = 0.62
    private const val EYE_HALF_Y = 0.3
    private const val EYE_GRID_X = 18
    private const val EYE_GRID_Y = 10
    private const val DARK_RATIO = 0.62
    internal const val STILL_SPANS_PER_SECOND = 2.2
    internal const val LOOK_LIMIT = 0.32
    internal const val REST_SECONDS = 3.0
    private const val TONGUE_GRID_X = 14
    private const val TONGUE_GRID_Y = 12
    internal const val TONGUE_GATE_LOW = 0.1
    internal const val TONGUE_GATE_HIGH = 0.24
    internal const val TONGUE_BASE_RISE_SECONDS = 10.0
    private val upperLids = listOf(37 to 41, 38 to 40, 43 to 47, 44 to 46)
    private const val LOWER_LIFT = 0.15
    val eyeCorners = listOf(36 to 39, 42 to 45)

    fun lumaOf(r: Double, g: Double, b: Double): Double = 0.299 * r + 0.587 * g + 0.114 * b

    fun rednessOf(r: Double, g: Double, b: Double): Double = (r - g) / (r + g + b + 12)

    fun eyeBox(points: List<CallFacePoint>, side: Int): CallFaceBox {
        val (a, b) = eyeCorners[side]
        val p = points[a]
        val q = points[b]
        val dx = q.x - p.x
        val dy = q.y - p.y
        val width = CallFaceJS.or(CallFaceJS.hypot(dx, dy), 1.0)
        val ux = CallFacePoint(dx / width, dy / width)
        val uy = CallFacePoint(-ux.y, ux.x)
        val lids = CallFaceGeometry.centroid(points, if (side == 1) listOf(43, 44, 46, 47) else listOf(37, 38, 40, 41))
        val mid = CallFacePoint((p.x + q.x) / 2, (p.y + q.y) / 2)
        val lift = (lids.x - mid.x) * uy.x + (lids.y - mid.y) * uy.y
        return CallFaceBox(
            CallFacePoint(mid.x + uy.x * lift * 0.5, mid.y + uy.y * lift * 0.5),
            ux,
            uy,
            CallFacePoint(EYE_HALF_X * width, EYE_HALF_Y * width),
        )
    }

    fun tongueWindow(points: List<CallFacePoint>): CallFaceBox {
        val frame = CallFaceGeometry.faceFrame(points)
        val lip = CallFacePoint((points[66].x + points[57].x) / 2, (points[66].y + points[57].y) / 2)
        val chin = points[8]
        val down = CallFacePoint(chin.x - lip.x, chin.y - lip.y)
        val reach = CallFaceJS.or(CallFaceJS.hypot(down.x, down.y), frame.span * 0.6)
        val mouth = CallFaceJS.or(CallFaceJS.hypot(points[54].x - points[48].x, points[54].y - points[48].y), frame.span * 0.8)
        val uy = CallFacePoint(down.x / reach, down.y / reach)
        return CallFaceBox(
            CallFacePoint(lip.x + down.x * 0.38, lip.y + down.y * 0.38),
            CallFacePoint(-uy.y, uy.x),
            uy,
            CallFacePoint(mouth * 0.42, reach * 0.55),
        )
    }

    fun sampleBox(image: CallFaceImage, box: CallFaceBox, gridX: Int, gridY: Int, visit: (Double, Double, Double, Double, Double) -> Unit) {
        for (j in 0 until gridY) {
            val v = ((j + 0.5) / gridY) * 2 - 1
            for (i in 0 until gridX) {
                val u = ((i + 0.5) / gridX) * 2 - 1
                val x = CallFaceJS.round(box.center.x + box.ux.x * u * box.half.x + box.uy.x * v * box.half.y)
                val y = CallFaceJS.round(box.center.y + box.ux.y * u * box.half.x + box.uy.y * v * box.half.y)
                if (!(x >= 0 && y >= 0 && x < image.width && y < image.height)) continue
                val at = (y.toInt() * image.width + x.toInt()) * 4
                visit(image.channel(at), image.channel(at + 1), image.channel(at + 2), u, v)
            }
        }
    }

    fun skinReference(image: CallFaceImage, points: List<CallFacePoint>): CallFaceSkin? {
        val frame = CallFaceGeometry.faceFrame(points)
        var luma = 0.0
        var red = 0.0
        var count = 0
        for ((a, b) in listOf(2 to 31, 14 to 35, 3 to 48, 13 to 54)) {
            val center = CallFacePoint((points[a].x + points[b].x) / 2, (points[a].y + points[b].y) / 2)
            val box = CallFaceBox(center, frame.ux, frame.uy, CallFacePoint(frame.span * 0.1, frame.span * 0.1))
            sampleBox(image, box, 5, 5) { r, g, bl, _, _ ->
                luma += lumaOf(r, g, bl)
                red += rednessOf(r, g, bl)
                count += 1
            }
        }
        return if (count > 0) CallFaceSkin(luma / count, red / count) else null
    }

    fun measureEye(image: CallFaceImage, box: CallFaceBox, skin: CallFaceSkin): CallFaceEyeReading? {
        val threshold = skin.luma * DARK_RATIO
        val grid = FloatArray(EYE_GRID_X * EYE_GRID_Y) { -1f }
        var total = 0
        var dark = 0
        var wx = 0.0
        var wy = 0.0
        var weight = 0.0
        sampleBox(image, box, EYE_GRID_X, EYE_GRID_Y) { r, g, b, u, v ->
            total += 1
            val luma = lumaOf(r, g, b)
            val row = CallFaceJS.round(((v + 1) / 2) * EYE_GRID_Y - 0.5)
            val column = CallFaceJS.round(((u + 1) / 2) * EYE_GRID_X - 0.5)
            grid[row.toInt() * EYE_GRID_X + column.toInt()] = luma.toFloat()
            if (luma >= threshold) return@sampleBox
            dark += 1
            val w = (threshold - luma) * (threshold - luma)
            wx += w * u
            wy += w * v
            weight += w
        }
        if (total == 0) return null
        var edge = 0.0
        var edges = 0
        for (j in 1 until EYE_GRID_Y - 1) {
            for (i in 1 until EYE_GRID_X - 1) {
                val left = grid[j * EYE_GRID_X + i - 1].toDouble()
                val right = grid[j * EYE_GRID_X + i + 1].toDouble()
                if (left < 0 || right < 0) continue
                edge += Math.abs(right - left)
                edges += 1
            }
        }
        return CallFaceEyeReading(
            dark.toDouble() / total,
            if (edges > 0) edge / edges / CallFaceJS.max(skin.luma, 1.0) else 0.0,
            if (weight > 0) CallFacePoint(wx / weight, wy / weight) else CallFacePoint(0.0, 0.0),
        )
    }

    fun measureTongue(image: CallFaceImage, box: CallFaceBox, skin: CallFaceSkin): Double {
        var total = 0
        var red = 0.0
        sampleBox(image, box, TONGUE_GRID_X, TONGUE_GRID_Y) { r, g, b, _, _ ->
            total += 1
            val luma = lumaOf(r, g, b)
            if (luma < skin.luma * 0.22 || luma > CallFaceJS.max(skin.luma * 2.4, 60.0)) return@sampleBox
            red += CallFaceGeometry.clampValue((rednessOf(r, g, b) - skin.red - 0.05) / 0.08, 0.0, 1.0)
        }
        return if (total > 0) red / total else 0.0
    }

    fun eyeOpenness(points: List<CallFacePoint>, side: Int): Double {
        val (a, b) = eyeCorners[side]
        val width = CallFaceJS.or(CallFaceJS.hypot(points[b].x - points[a].x, points[b].y - points[a].y), 1.0)
        val pairs = if (side == 1) listOf(43 to 47, 44 to 46) else listOf(37 to 41, 38 to 40)
        val gap = pairs.fold(0.0) { sum, (u, l) -> sum + CallFaceJS.hypot(points[u].x - points[l].x, points[u].y - points[l].y) }
        return gap / pairs.size / width
    }

    fun pairClosure(close: DoubleArray): DoubleArray {
        val low = CallFaceJS.min(close[0], close[1])
        return DoubleArray(close.size) { low + (close[it] - low) * (1 - 0.75) }
    }

    fun closeLids(points: List<CallFacePoint>, close: DoubleArray?): List<CallFacePoint> {
        if (close == null || !(close[0] > 0.01 || close[1] > 0.01)) return points
        val out = points.toMutableList()
        upperLids.forEachIndexed { k, (upper, lower) ->
            val amount = CallFaceGeometry.clampValue(close[if (k < 2) 0 else 1], 0.0, 1.0)
            if (amount <= 0.01) return@forEachIndexed
            val meetX = out[lower].x + (out[upper].x - out[lower].x) * LOWER_LIFT
            val meetY = out[lower].y + (out[upper].y - out[lower].y) * LOWER_LIFT
            out[upper] = CallFacePoint(out[upper].x + (meetX - out[upper].x) * amount, out[upper].y + (meetY - out[upper].y) * amount, out[upper].z)
            out[lower] = CallFacePoint(out[lower].x + (meetX - out[lower].x) * amount, out[lower].y + (meetY - out[lower].y) * amount, out[lower].z)
        }
        return out
    }

    fun probeCrop(points: List<CallFacePoint>): CallFaceProbeCrop {
        val frame = CallFaceGeometry.faceFrame(points)
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
        val pad = frame.span * 0.3
        minX = Math.floor(minX - pad)
        minY = Math.floor(minY - pad)
        maxX = Math.ceil(maxX + pad)
        maxY = Math.ceil(maxY + pad)
        val scale = CallFaceJS.min(1.0, PROBE_SPAN / frame.span)
        val width = CallFaceJS.max(4.0, CallFaceJS.round((maxX - minX) * scale)).toInt()
        val height = CallFaceJS.max(4.0, CallFaceJS.round((maxY - minY) * scale)).toInt()
        return CallFaceProbeCrop(minX, minY, scale, width, height)
    }
}

class CallFaceBlinkTracker {
    private class Eye {
        var base = 0.0
        var lidBase = 0.0
        var closing = false
        val close = CallFaceOneEuroFilter(4.0, 2.0)
    }

    private val eyes = listOf(Eye(), Eye())

    fun reset() {
        for (eye in eyes) {
            eye.base = 0.0
            eye.lidBase = 0.0
            eye.closing = false
            eye.close.reset()
        }
    }

    fun update(input: CallFaceBlinkInput): DoubleArray {
        val raw = doubleArrayOf(0.0, 0.0)
        for (side in 0 until 2) {
            val eye = eyes[side]
            val edge = input.edges[side]
            if (!edge.isFinite()) continue
            var gate = 1.0
            val openness = input.openness
            if (openness != null && openness[side].isFinite()) {
                val open = openness[side]
                if (!CallFaceJS.truthy(eye.lidBase)) {
                    eye.lidBase = open
                } else if (open >= eye.lidBase) {
                    eye.lidBase += (open - eye.lidBase) * clamp(input.elapsed / LID_BASE_RISE_SECONDS, 0.0, 1.0)
                } else if (!eye.closing) {
                    eye.lidBase -= (eye.lidBase - open) * clamp(input.elapsed / LID_BASE_FALL_SECONDS, 0.0, 1.0)
                }
                if (eye.lidBase >= LID_MIN) gate = clamp((LID_OPEN - open / eye.lidBase) / (LID_OPEN - LID_SHUT), 0.0, 1.0)
            }
            if (!CallFaceJS.truthy(eye.base)) {
                eye.base = edge
            } else if (edge >= eye.base) {
                eye.base += (edge - eye.base) * clamp(input.elapsed / BASE_RISE_SECONDS, 0.0, 1.0)
            } else if (!eye.closing) {
                eye.base -= (eye.base - edge) * clamp(input.elapsed / BASE_FALL_SECONDS, 0.0, 1.0)
            }
            var amount = 0.0
            if (eye.base >= OPEN_MIN) amount = clamp((CLOSE_START - edge / eye.base) / (CLOSE_START - CLOSE_FULL), 0.0, 1.0)
            raw[side] = if (input.moving) 0.0 else amount * gate
        }
        val paired = CallFaceExpression.pairClosure(raw)
        return DoubleArray(2) { side ->
            val eye = eyes[side]
            if (!input.edges[side].isFinite()) return@DoubleArray 0.0
            var amount = paired[side]
            if (if (eye.closing) amount < LEAVE else amount < ENTER) {
                eye.closing = false
                amount = 0.0
            } else {
                eye.closing = true
            }
            clamp(eye.close.filter(amount, input.time), 0.0, 1.0)
        }
    }

    private fun clamp(value: Double, low: Double, high: Double): Double = CallFaceGeometry.clampValue(value, low, high)

    companion object {
        private const val OPEN_MIN = 0.05
        private const val CLOSE_START = 0.52
        private const val CLOSE_FULL = 0.28
        private const val BASE_RISE_SECONDS = 0.8
        private const val BASE_FALL_SECONDS = 6.0
        private const val LID_OPEN = 0.9
        private const val LID_SHUT = 0.6
        private const val LID_BASE_RISE_SECONDS = 1.5
        private const val LID_BASE_FALL_SECONDS = 8.0
        private const val LID_MIN = 0.12
        private const val ENTER = 0.4
        private const val LEAVE = 0.18
    }
}

class CallFaceExpressionProbe {
    private class Eye {
        val lookX = CallFaceOneEuroFilter(1.2, 0.8)
        val lookY = CallFaceOneEuroFilter(1.2, 0.8)
        var look = CallFacePoint(0.0, 0.0)
        var rest: CallFacePoint? = null
    }

    private val blinks = CallFaceBlinkTracker()
    private val eyes = listOf(Eye(), Eye())
    private var tongueBase = -1.0
    private val tongueGate = CallFaceOneEuroFilter(3.0, 1.0)
    private var last = 0.0
    private var lastMid: CallFacePoint? = null

    fun reset() {
        lastMid = null
        blinks.reset()
        for (eye in eyes) {
            eye.lookX.reset()
            eye.lookY.reset()
            eye.look = CallFacePoint(0.0, 0.0)
            eye.rest = null
        }
        tongueBase = -1.0
        tongueGate.reset()
        last = 0.0
    }

    fun measure(
        image: CallFaceImage,
        crop: CallFaceProbeCrop,
        points: List<CallFacePoint>,
        time: Double,
        pupils: List<CallFacePoint>? = null,
        lids: Boolean = false,
    ): CallFaceExpressionReading? {
        val e = CallFaceExpression
        fun clamp(value: Double, low: Double, high: Double): Double = CallFaceGeometry.clampValue(value, low, high)
        val local = points.map(crop::local)
        val skin = e.skinReference(image, local) ?: return null
        if (skin.luma < 18) return null
        val elapsed = if (CallFaceJS.truthy(last)) clamp((time - last) / 1000, 0.0, 1.0) else 0.0
        last = time
        val frame = CallFaceGeometry.faceFrame(points)
        val previous = lastMid
        val speed = if (previous != null && elapsed > 0) CallFaceJS.hypot(frame.mid.x - previous.x, frame.mid.y - previous.y) / frame.span / elapsed else 0.0
        lastMid = frame.mid
        val boxes = listOf(e.eyeBox(local, 0), e.eyeBox(local, 1))
        val found = boxes.map { e.measureEye(image, it, skin) }
        val close = blinks.update(CallFaceBlinkInput(
            edges = DoubleArray(2) { found[it]?.edge ?: Double.NaN },
            openness = if (lids) doubleArrayOf(e.eyeOpenness(points, 0), e.eyeOpenness(points, 1)) else null,
            elapsed = elapsed,
            time = time,
            moving = speed > CallFaceExpression.STILL_SPANS_PER_SECOND,
        ))
        val look = mutableListOf(CallFacePoint(0.0, 0.0), CallFacePoint(0.0, 0.0))
        for (side in 0 until 2) {
            val eye = eyes[side]
            val seen = found[side] ?: continue
            val box = boxes[side]
            var gazeAt = seen.look
            if (pupils != null && side < pupils.size) {
                val pupil = pupils[side]
                val px = (pupil.x - crop.minX) * crop.scale - box.center.x
                val py = (pupil.y - crop.minY) * crop.scale - box.center.y
                gazeAt = CallFacePoint((px * box.ux.x + py * box.ux.y) / box.half.x, (px * box.uy.x + py * box.uy.y) / box.half.y)
            }
            if (close[side] < 0.4) {
                val rest = eye.rest ?: CallFacePoint(gazeAt.x, gazeAt.y)
                val pull = clamp(elapsed / CallFaceExpression.REST_SECONDS, 0.0, 1.0)
                val settled = CallFacePoint(rest.x + (gazeAt.x - rest.x) * pull, rest.y + (gazeAt.y - rest.y) * pull)
                eye.rest = settled
                val limit = CallFaceExpression.LOOK_LIMIT
                eye.look = CallFacePoint(
                    clamp(eye.lookX.filter(gazeAt.x - settled.x, time), -limit, limit),
                    clamp(eye.lookY.filter(gazeAt.y - settled.y, time), -limit, limit),
                )
            }
            look[side] = eye.look
        }
        val mouthBox = e.tongueWindow(local)
        val red = e.measureTongue(image, mouthBox, skin)
        if (tongueBase < 0) {
            tongueBase = red
        } else if (red < tongueBase) {
            tongueBase += (red - tongueBase) * 0.5
        } else {
            tongueBase += (red - tongueBase) * (elapsed / CallFaceExpression.TONGUE_BASE_RISE_SECONDS)
        }
        val raised = red - tongueBase
        val gate = clamp((raised - CallFaceExpression.TONGUE_GATE_LOW) / (CallFaceExpression.TONGUE_GATE_HIGH - CallFaceExpression.TONGUE_GATE_LOW), 0.0, 1.0)
        return CallFaceExpressionReading(
            close = close,
            look = look,
            tongue = clamp(tongueGate.filter(gate, time), 0.0, 1.0),
            skinRed = skin.red,
            skinLuma = skin.luma / 255,
        )
    }
}
