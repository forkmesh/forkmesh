package com.forkmesh.nativeapp.chat.face

data class CallFaceCustomTransform(val scale: Double, val matrix: DoubleArray)

data class CallFaceCustomPolygons(val face: List<CallFacePoint>, val inner: List<CallFacePoint>, val neck: List<CallFacePoint>)

data class CallFaceSkinModel(val mean: DoubleArray, val inverse: DoubleArray)

class CallFaceCustomMatte(val matte: FloatArray, val skin: FloatArray, val hair: FloatArray, val head: List<Double>)

class CallFaceCustomException(val reason: String) : Exception(reason)

object CallFaceCustom {
    const val ID = "custom"
    const val LABEL = "Your photo"
    const val STORAGE_KEY = "forkmesh.chat.callFaceImage"
    const val SIDE = 1024.0
    const val SIZE = 512
    private const val EYE_SPAN = 118.0
    private const val EYE_MID_X = 256.0
    private const val EYE_MID_Y = 285.0
    private const val RING = 28

    fun transform(points: List<CallFacePoint>): CallFaceCustomTransform {
        val frame = CallFaceGeometry.faceFrame(points)
        val scale = EYE_SPAN / frame.span
        val c = StrictMath.cos(-frame.roll) * scale
        val s = StrictMath.sin(-frame.roll) * scale
        return CallFaceCustomTransform(
            scale,
            doubleArrayOf(c, s, -s, c, EYE_MID_X - (c * frame.mid.x - s * frame.mid.y), EYE_MID_Y - (s * frame.mid.x + c * frame.mid.y)),
        )
    }

    fun apply(matrix: DoubleArray, point: CallFacePoint): CallFacePoint =
        CallFacePoint(matrix[0] * point.x + matrix[2] * point.y + matrix[4], matrix[1] * point.x + matrix[3] * point.y + matrix[5])

    fun smoothstep(edge0: Double, edge1: Double, x: Double): Double {
        val t = CallFaceGeometry.clampValue((x - edge0) / (edge1 - edge0), 0.0, 1.0)
        return t * t * (3 - 2 * t)
    }

    fun boxBlur(values: FloatArray, size: Int, radius: Int, passes: Int = 1): FloatArray {
        var source = values
        val divisor = (2 * radius + 1).toDouble()
        repeat(passes) {
            val temp = FloatArray(values.size)
            val out = FloatArray(values.size)
            for (y in 0 until size) {
                var sum = 0.0
                val row = y * size
                for (x in -radius..radius) sum += source[row + minOf(maxOf(x, 0), size - 1)].toDouble()
                for (x in 0 until size) {
                    temp[row + x] = (sum / divisor).toFloat()
                    sum += source[row + minOf(size - 1, x + radius + 1)].toDouble() - source[row + maxOf(0, x - radius)].toDouble()
                }
            }
            for (x in 0 until size) {
                var sum = 0.0
                for (y in -radius..radius) sum += temp[minOf(maxOf(y, 0), size - 1) * size + x].toDouble()
                for (y in 0 until size) {
                    out[y * size + x] = (sum / divisor).toFloat()
                    sum += temp[minOf(size - 1, y + radius + 1) * size + x].toDouble() - temp[maxOf(0, y - radius) * size + x].toDouble()
                }
            }
            source = out
        }
        return source
    }

    fun headRing(matte: FloatArray, size: Int, mid: CallFacePoint, span: Double): List<Double> {
        val center = CallFacePoint(mid.x, mid.y - 0.35 * span)
        val reaches = ArrayList<Int>()
        val directions = ArrayList<CallFacePoint>()
        val limit = (size - 1).toDouble()
        for (k in 0 until RING) {
            val angle = (2 * Math.PI * k) / RING - Math.PI / 2
            val direction = CallFacePoint(StrictMath.cos(angle), StrictMath.sin(angle))
            var reach = 0
            var r = 0
            while (true) {
                val x = center.x + direction.x * r
                val y = center.y + direction.y * r
                if (!(x >= 0 && x < limit && y >= 0 && y < limit)) break
                if (matte[Math.floor(y).toInt() * size + Math.floor(x).toInt()].toDouble() > 0.03) reach = r
                r += 1
            }
            reaches.add(reach)
            directions.add(direction)
        }
        fun at(i: Int): Int = reaches[(i + RING) % RING]
        val widened = reaches.indices.map { maxOf(at(it - 1), at(it), at(it + 1)) }
        fun wide(i: Int): Int = widened[(i + RING) % RING]
        val out = ArrayList<Double>()
        reaches.forEachIndexed { i, reach ->
            val smooth = (wide(i - 2) + wide(i - 1) + wide(i) + wide(i + 1) + wide(i + 2)).toDouble() / 5
            val radius = CallFaceJS.max(smooth, reach.toDouble(), 0.9 * span) + 0.18 * span
            val x = CallFaceGeometry.clampValue(center.x + directions[i].x * radius, 0.0, size.toDouble())
            val y = CallFaceGeometry.clampValue(center.y + directions[i].y * radius, 0.0, size.toDouble())
            out.add(CallFaceJS.round(x * 10) / 10)
            out.add(CallFaceJS.round(y * 10) / 10)
        }
        return out
    }

    fun polygons(p: List<CallFacePoint>): CallFaceCustomPolygons {
        val left = CallFaceGeometry.centroid(p, listOf(36, 37, 38, 39, 40, 41))
        val right = CallFaceGeometry.centroid(p, listOf(42, 43, 44, 45, 46, 47))
        val span = CallFaceJS.or(CallFaceJS.hypot(right.x - left.x, right.y - left.y), EYE_SPAN)
        val forehead = (26 downTo 17).map { CallFacePoint(p[it].x, p[it].y - span * 1.2) }
        val face = p.subList(0, 17) + forehead
        val inner = p.subList(0, 17) + p.subList(17, 27).reversed().map { CallFacePoint(it.x, it.y - span * 0.15) }
        val chin = p[8]
        val neckHalf = CallFaceJS.hypot(p[4].x - p[12].x, p[4].y - p[12].y) * 0.5
        val neck = listOf(p[4], p[12], CallFacePoint(p[12].x + neckHalf * 0.08, chin.y + span * 1.1), CallFacePoint(p[4].x - neckHalf * 0.08, chin.y + span * 1.1))
        return CallFaceCustomPolygons(face, inner, neck)
    }

    fun rasterize(polygon: List<CallFacePoint>, size: Int): ByteArray {
        val out = ByteArray(size * size)
        for (y in 0 until size) {
            for (x in 0 until size) {
                if (CallFaceGeometry.insidePolygon(polygon, CallFacePoint(x + 0.5, y + 0.5))) out[y * size + x] = 255.toByte()
            }
        }
        return out
    }

    private fun chromaU(r: Double, g: Double, b: Double): Double = r / (r + g + b + 7.65)

    private fun chromaV(r: Double, g: Double, b: Double): Double = g / (r + g + b + 7.65)

    private fun byte(pixels: ByteArray, at: Int): Double = (pixels[at].toInt() and 0xff).toDouble()

    fun skinModel(pixels: ByteArray, size: Int, points: List<CallFacePoint>, span: Double): CallFaceSkinModel? {
        val us = ArrayList<Double>()
        val vs = ArrayList<Double>()
        val radius = CallFaceJS.max(2.0, CallFaceJS.round(span * 0.12)).toInt()
        for ((a, b) in listOf(2 to 31, 14 to 35, 3 to 48, 13 to 54)) {
            val cx = CallFaceJS.round((points[a].x + points[b].x) / 2).toInt()
            val cy = CallFaceJS.round((points[a].y + points[b].y) / 2).toInt()
            for (y in cy - radius until cy + radius) {
                for (x in cx - radius until cx + radius) {
                    if (x < 0 || y < 0 || x >= size || y >= size) continue
                    val at = (y * size + x) * 4
                    val r = byte(pixels, at)
                    val g = byte(pixels, at + 1)
                    val bl = byte(pixels, at + 2)
                    us.add(chromaU(r, g, bl))
                    vs.add(chromaV(r, g, bl))
                }
            }
        }
        if (us.size < 8) return null
        val n = us.size.toDouble()
        val mean = doubleArrayOf(0.0, 0.0)
        for (k in us.indices) {
            mean[0] += us[k] / n
            mean[1] += vs[k] / n
        }
        var a = 2e-5
        var b = 0.0
        var d = 2e-5
        for (k in us.indices) {
            val u = us[k]
            val v = vs[k]
            a += ((u - mean[0]) * (u - mean[0])) / n
            b += ((u - mean[0]) * (v - mean[1])) / n
            d += ((v - mean[1]) * (v - mean[1])) / n
        }
        val det = CallFaceJS.or(a * d - b * b, 1e-12)
        return CallFaceSkinModel(mean, doubleArrayOf(d / det, -b / det, a / det))
    }

    fun skinLikeness(pixels: ByteArray, size: Int, model: CallFaceSkinModel?): FloatArray {
        val out = FloatArray(size * size)
        if (model == null) return out
        val ia = model.inverse[0]
        val ib = model.inverse[1]
        val id = model.inverse[2]
        for (i in out.indices) {
            val r = byte(pixels, i * 4)
            val g = byte(pixels, i * 4 + 1)
            val bl = byte(pixels, i * 4 + 2)
            val du = chromaU(r, g, bl) - model.mean[0]
            val dv = chromaV(r, g, bl) - model.mean[1]
            val distance = du * du * ia + 2 * du * dv * ib + dv * dv * id
            out[i] = (StrictMath.exp(-0.5 * distance / 9) * smoothstep(0.03, 0.12, (r + g + bl) / 765)).toFloat()
        }
        return out
    }

    fun maskValues(bytes: ByteArray): FloatArray = FloatArray(bytes.size) { ((bytes[it].toInt() and 0xff).toDouble() / 255).toFloat() }

    fun personWeights(bytes: ByteArray): FloatArray = FloatArray(bytes.size) { smoothstep(0.2, 0.8, (bytes[it].toInt() and 0xff).toDouble() / 255).toFloat() }

    fun buildMatte(
        pixels: ByteArray,
        size: Int = SIZE,
        points: List<CallFacePoint>,
        person: FloatArray? = null,
        faceMask: ByteArray,
        innerMask: ByteArray,
        neckMask: ByteArray,
    ): CallFaceCustomMatte {
        val p = points
        val left = CallFaceGeometry.centroid(p, listOf(36, 37, 38, 39, 40, 41))
        val right = CallFaceGeometry.centroid(p, listOf(42, 43, 44, 45, 46, 47))
        val span = CallFaceJS.or(CallFaceJS.hypot(right.x - left.x, right.y - left.y), EYE_SPAN)
        val center = CallFacePoint((left.x + right.x) / 2, (left.y + right.y) / 2)
        val chin = p[8]
        val face = boxBlur(maskValues(faceMask), size, 3, 2)
        val inner = boxBlur(maskValues(innerMask), size, 2, 2)
        val neckRaw = boxBlur(maskValues(neckMask), size, 5, 2)
        val faceWide = boxBlur(face, size, 6, 2)
        val likeness = boxBlur(skinLikeness(pixels, size, skinModel(pixels, size, p, span)), size, 2, 1)
        val matte = FloatArray(size * size)
        val neck = FloatArray(size * size)
        val full = size.toDouble()
        for (yi in 0 until size) {
            val y = yi.toDouble()
            val below = smoothstep(chin.y - span * 0.1, chin.y + span * 0.2, y)
            val fadeNeck = 1 - smoothstep(chin.y + span * 0.1, chin.y + span * 0.7, y)
            val above = 1 - smoothstep(p[4].y, p[5].y + span * 0.25, y)
            val bottom = 1 - smoothstep(full - span * 0.55, full - span * 0.05, y)
            for (xi in 0 until size) {
                val x = xi.toDouble()
                val i = yi * size + xi
                neck[i] = (neckRaw[i].toDouble() * fadeNeck).toFloat()
                val reach = CallFaceJS.hypot((x - center.x) / (span * 1.75), (y - center.y + span * 0.4) / (span * 2.3))
                val near = 1 - smoothstep(0.85, 1.05, reach)
                val hairReach = CallFaceJS.hypot((x - center.x) / (span * 1.9), (y - center.y - span * 0.2) / (span * 2.9))
                val hairZone = (1 - smoothstep(0.85, 1.05, hairReach)) * (1 - smoothstep(chin.y, chin.y + span * 0.4, y))
                val oval = 1 - smoothstep(0.8, 1.0, CallFaceJS.hypot((x - center.x) / (span * 1.3), (y - center.y + span * 0.45) / (span * 1.95)))
                val region = if (person != null) {
                    CallFaceJS.max(above * near, face[i].toDouble(), neck[i].toDouble(), hairZone)
                } else {
                    CallFaceJS.max(face[i].toDouble(), neck[i].toDouble(), oval)
                }
                val who = person?.get(i)?.toDouble() ?: 1.0
                var value = CallFaceGeometry.clampValue(who * region, 0.0, 1.0)
                val keepNeck = CallFaceJS.max(smoothstep(0.08, 0.35, likeness[i].toDouble()), face[i].toDouble())
                value *= 1 - below + below * keepNeck
                matte[i] = (value * bottom).toFloat()
            }
        }
        val soft = boxBlur(matte, size, 1, 1)
        val skin = FloatArray(size * size)
        val hair = FloatArray(size * size)
        for (i in soft.indices) {
            val m = CallFaceJS.max(soft[i].toDouble(), if (matte[i].toDouble() > 0.97) 1.0 else 0.0) *
                (if (matte[i].toDouble() > 0.001 || soft[i].toDouble() > 0.05) 1.0 else 0.0)
            soft[i] = m.toFloat()
            val skinLike = smoothstep(0.2, 0.55, likeness[i].toDouble())
            skin[i] = CallFaceGeometry.clampValue(m * CallFaceJS.max(skinLike * CallFaceJS.max(faceWide[i].toDouble(), neck[i].toDouble()), inner[i].toDouble() * 0.85), 0.0, 1.0).toFloat()
            hair[i] = CallFaceGeometry.clampValue(m * (1 - skinLike) * (1 - inner[i].toDouble()), 0.0, 1.0).toFloat()
        }
        return CallFaceCustomMatte(soft, boxBlur(skin, size, 2, 1), boxBlur(hair, size, 1, 1), headRing(soft, size, center, span))
    }

    fun matteRGBA(built: CallFaceCustomMatte): ByteArray {
        val out = ByteArray(built.matte.size * 4) { 255.toByte() }
        fun byte(v: Float): Byte = CallFaceGeometry.clampValue(CallFaceJS.round(v.toDouble() * 255), 0.0, 255.0).toInt().toByte()
        for (i in built.matte.indices) {
            out[i * 4] = byte(built.matte[i])
            out[i * 4 + 1] = byte(built.skin[i])
            out[i * 4 + 2] = byte(built.hair[i])
        }
        return out
    }

    fun validate(points: List<CallFacePoint>?): List<CallFacePoint> {
        if (points == null || points.size < CallFaceGeometry.LANDMARK_COUNT) throw CallFaceCustomException("face_not_found")
        if (!(CallFaceGeometry.faceFrame(points).span >= 12)) throw CallFaceCustomException("face_too_small")
        return points
    }

    fun face(placed: List<CallFacePoint>, head: List<Double>): CallFaceModel =
        CallFaceModel(ID, LABEL, placed.flatMap { listOf(CallFaceJS.round(it.x * 10) / 10, CallFaceJS.round(it.y * 10) / 10) }, head)
}
