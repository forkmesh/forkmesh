package com.forkmesh.nativeapp.chat.face

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText

@Suppress("UNCHECKED_CAST")
object CallFaceParity {
    const val TOLERANCE = 1e-6

    val fixture: Map<String, Any?> by lazy { Json.parseObject(resourceText("chat/call-face-parity.json")) }

    val worst = LinkedHashMap<String, Double>()
    val hashes = LinkedHashMap<String, Pair<Int, Int>>()

    fun obj(value: Any?): Map<String, Any?> = value as Map<String, Any?>
    fun list(value: Any?): List<Any?> = value as List<Any?>
    fun num(value: Any?): Double = if (value == null) Double.NaN else (value as Number).toDouble()
    fun int(value: Any?): Int = (value as Number).toInt()
    fun nums(value: Any?): List<Double> = list(value).map(::num)
    fun at(value: Any?, vararg path: Any): Any? {
        var current = value
        for (key in path) current = if (key is String) obj(current)[key] else list(current)[key as Int]
        return current
    }

    fun close(section: String, expected: Any?, actual: Double, label: String) {
        val e = num(expected)
        if (e.isNaN() || !actual.isFinite()) {
            expect(e.isNaN() && !actual.isFinite() || (e.isNaN() && actual.isNaN()), "$label: expected $e but was $actual")
            return
        }
        val error = Math.abs(actual - e) / maxOf(1.0, Math.abs(e))
        worst[section] = maxOf(worst[section] ?: 0.0, error)
        expect(error <= TOLERANCE, "$section $label: expected $e but was $actual")
    }

    fun numbers(section: String, expected: Any?, actual: DoubleArray, label: String) {
        val e = list(expected)
        assertEq(e.size, actual.size, "$label length")
        e.forEachIndexed { i, v -> close(section, v, actual[i], "$label[$i]") }
    }

    fun numbers(section: String, expected: Any?, actual: List<Double>, label: String) = numbers(section, expected, actual.toDoubleArray(), label)

    fun point(section: String, expected: Any?, actual: CallFacePoint, label: String) = numbers(section, expected, actual.values, label)

    fun points(section: String, expected: Any?, actual: List<CallFacePoint>, label: String) {
        val e = list(expected)
        assertEq(e.size, actual.size, "$label count")
        e.forEachIndexed { i, v -> point(section, v, actual[i], "$label[$i]") }
    }

    fun frame(section: String, expected: Any?, f: CallFaceFrame, label: String) {
        point(section, at(expected, "left"), f.left, "$label.left")
        point(section, at(expected, "right"), f.right, "$label.right")
        point(section, at(expected, "mid"), f.mid, "$label.mid")
        close(section, at(expected, "span"), f.span, "$label.span")
        point(section, at(expected, "ux"), f.ux, "$label.ux")
        point(section, at(expected, "uy"), f.uy, "$label.uy")
        close(section, at(expected, "roll"), f.roll, "$label.roll")
    }

    fun pose(section: String, expected: Any?, p: CallFacePose, label: String) {
        close(section, at(expected, "scale"), p.scale, "$label.scale")
        list(at(expected, "rows")).forEachIndexed { i, row -> numbers(section, row, p.rows[i], "$label.rows[$i]") }
        numbers(section, at(expected, "t"), p.t, "$label.t")
        close(section, at(expected, "roll"), p.roll, "$label.roll")
        close(section, at(expected, "yaw"), p.yaw, "$label.yaw")
        close(section, at(expected, "pitch"), p.pitch, "$label.pitch")
        assertEq((obj(expected)["depth"] as Boolean?) ?: false, p.depth, "$label.depth")
    }

    fun box(section: String, expected: Any?, b: CallFaceBox, label: String) {
        point(section, at(expected, "center"), b.center, "$label.center")
        point(section, at(expected, "ux"), b.ux, "$label.ux")
        point(section, at(expected, "uy"), b.uy, "$label.uy")
        point(section, at(expected, "half"), b.half, "$label.half")
    }

    fun axis(section: String, expected: Any?, f: CallFaceAxisFrame, label: String) {
        point(section, at(expected, "center"), f.center, "$label.center")
        point(section, at(expected, "x"), f.x, "$label.x")
        point(section, at(expected, "y"), f.y, "$label.y")
    }

    fun outfit(section: String, expected: Any?, f: CallOutfitFrame?, label: String) {
        if (expected == null) {
            expect(f == null, "$label should be null")
            return
        }
        expect(f != null, "$label missing")
        point(section, at(expected, "center"), f!!.center, "$label.center")
        point(section, at(expected, "x"), f.x, "$label.x")
        point(section, at(expected, "y"), f.y, "$label.y")
        close(section, at(expected, "unit"), f.unit, "$label.unit")
        close(section, at(expected, "roll"), f.roll, "$label.roll")
    }

    fun fnv(values: FloatArray): Long {
        var h = 0x811c9dc5.toInt()
        for (v in values) h = (h xor v.toRawBits()) * 0x01000193
        return h.toLong() and 0xffffffffL
    }

    fun fnvBytes(values: ByteArray): Long {
        var h = 0x811c9dc5.toInt()
        for (v in values) h = (h xor (v.toInt() and 0xff)) * 0x01000193
        return h.toLong() and 0xffffffffL
    }

    fun countHash(section: String, same: Boolean) {
        val (a, b) = hashes[section] ?: (0 to 0)
        hashes[section] = (a + if (same) 1 else 0) to (b + 1)
    }

    fun floats(section: String, expected: Any?, actual: FloatArray, stride: Int, label: String) {
        assertEq(int(at(expected, "length")), actual.size, "$label length")
        var sum = 0.0
        for (v in actual) sum += v.toDouble()
        close(section, at(expected, "sum"), sum, "$label.sum")
        nums(at(expected, "samples")).forEachIndexed { k, v -> close(section, v, actual[k * stride].toDouble(), "$label[${k * stride}]") }
        countHash(section, num(at(expected, "hash")).toLong() == fnv(actual))
    }

    fun <T> strided(items: List<T>, stride: Int): List<T> = items.indices.step(stride).map { items[it] }

    fun stridedArray(items: DoubleArray, stride: Int): DoubleArray = items.indices.step(stride).map { items[it] }.toDoubleArray()

    fun regions(value: Any?): CallFaceRegions? = (value as Map<String, Any?>?)?.mapValues { (_, region) -> list(region).map(::nums) }

    fun pointList(value: Any?): List<CallFacePoint> = list(value).map { CallFacePoint.of(nums(it)) }

    fun head(section: String, expected: Any?, head: CallFaceLiveHead, entry: CallFaceHead, points: List<CallFacePoint>, stride: Int, label: String) {
        val depths = CallFaceGeometry.vertexDepths(entry.model, head.pose)
        val warp = CallFaceWarp.warpVertices(entry.mesh, entry.layout, head.points, 512.0, depths)
        pose(section, at(expected, "pose"), head.pose, "$label.pose")
        numbers(section, at(expected, "jawTrust"), head.jawTrust, "$label.jawTrust")
        points(section, at(expected, "points"), strided(head.points, stride), "$label.points")
        numbers(section, at(expected, "depths"), stridedArray(depths, stride), "$label.depths")
        floats("$section.warp", at(expected, "warp"), warp, 17, "$label.warp")
        axis(section, at(expected, "zone"), CallFaceWarp.zoneFrame(points, head.pose), "$label.zone")
        val hair = CallFaceWarp.hairRegion(head.pose, 640.0, 480.0)
        point(section, at(expected, "hair", "center"), hair.center, "$label.hair.center")
        point(section, at(expected, "hair", "size"), hair.size, "$label.hair.size")
    }

    fun mesh(expected: Any?, mesh: CallFaceHeadMesh, label: String) {
        assertEq(int(at(expected, "count")), mesh.count, "$label.count")
        assertEq(int(at(expected, "extraStart")), mesh.extraStart, "$label.extraStart")
        assertEq(int(at(expected, "mouthStart")), mesh.mouthStart, "$label.mouthStart")
        assertEq(list(at(expected, "triangles")).map { t -> list(t).map(::int) }, mesh.triangles.map { it.v.toList() }, "$label.triangles")
        assertEq(list(at(expected, "holes")).map(::nums), mesh.triangles.map { it.hole.toList() }, "$label.holes")
        assertEq(list(at(expected, "mouth")), mesh.triangles.map { it.mouth }, "$label.mouth")
        assertEq(list(at(expected, "cull")), mesh.triangles.map { it.cull }, "$label.cull")
    }

    fun jsRound(v: Double): Double {
        val f = Math.floor(v)
        return if (v - f >= 0.5) f + 1 else f
    }

    fun fine(v: Double): Double = jsRound(v * 1000) / 1000

    fun xorshift(seed: Long): () -> Double {
        var s = seed.toInt()
        return {
            s = s xor (s shl 13)
            s = s xor (s ushr 17)
            s = s xor (s shl 5)
            (s.toLong() and 0xffffffffL).toDouble() / 4294967296.0
        }
    }

    class Pose(
        val yaw: Double = 0.0,
        val pitch: Double = 0.0,
        val roll: Double = 0.0,
        val scale: Double = 100.0,
        val cx: Double = 320.0,
        val cy: Double = 240.0,
        val open: Double = 0.0,
        val close: DoubleArray = doubleArrayOf(0.0, 0.0),
        val smile: Double = 0.0,
    )

    fun synthetic(pose: Pose): List<CallFacePoint> {
        val t = CallFaces.template.map { doubleArrayOf(it.x, it.y, it.z!!) }
        for ((i, w) in listOf(55 to 0.45, 56 to 0.8, 57 to 1.0, 58 to 0.8, 59 to 0.45, 65 to 0.8, 66 to 1.0, 67 to 0.8)) t[i][1] += pose.open * 0.35 * w
        for ((i, w) in listOf(5 to 0.3, 6 to 0.6, 7 to 0.85, 8 to 1.0, 9 to 0.85, 10 to 0.6, 11 to 0.3)) t[i][1] += pose.open * 0.25 * w
        t[48][0] -= pose.smile * 0.08
        t[48][1] -= pose.smile * 0.06
        t[54][0] += pose.smile * 0.08
        t[54][1] -= pose.smile * 0.06
        for ((u, l, side) in listOf(Triple(37, 41, 0), Triple(38, 40, 0), Triple(43, 47, 1), Triple(44, 46, 1))) {
            val a = pose.close[side] * 0.85
            t[u][0] += (t[l][0] - t[u][0]) * a
            t[u][1] += (t[l][1] - t[u][1]) * a
        }
        val cyw = StrictMath.cos(pose.yaw)
        val syw = StrictMath.sin(pose.yaw)
        val cp = StrictMath.cos(pose.pitch)
        val sp = StrictMath.sin(pose.pitch)
        val cr = StrictMath.cos(pose.roll)
        val sr = StrictMath.sin(pose.roll)
        return t.map { (x, y, z) ->
            val x1 = x * cyw + z * syw
            val z1 = -x * syw + z * cyw
            val y2 = y * cp - z1 * sp
            val z2 = y * sp + z1 * cp
            CallFacePoint(pose.cx + pose.scale * (x1 * cr - y2 * sr), pose.cy + pose.scale * (x1 * sr + y2 * cr), pose.scale * z2)
        }
    }

    fun loop(points: List<CallFacePoint>, count: Int, rotate: Int): List<CallFacePoint> {
        val out = CallFaceGeometry.resamplePolyline(points + points[0], count + 1).subList(0, count)
        return out.subList(rotate, out.size) + out.subList(0, rotate)
    }

    fun visionRegions(p: List<CallFacePoint>, depth: Boolean = false, medianLine: Boolean = false, reverseJaw: Boolean = false): CallFaceRegions {
        fun cut(q: CallFacePoint): CallFacePoint = if (depth) q else q.flat
        fun pick(idx: List<Int>): List<CallFacePoint> = idx.map { cut(p[it]) }
        fun shift(q: CallFacePoint, dy: Double): CallFacePoint = q.copy(y = q.y + dy)
        fun band(idx: List<Int>): List<CallFacePoint> = loop(pick(idx).map { shift(it, -3.0) } + pick(idx).map { shift(it, 3.0) }.reversed(), 6, 2)
        val jaw = CallFaceGeometry.resamplePolyline(pick((0 until 17).toList()), 11)
        val out = LinkedHashMap<String, List<CallFacePoint>>()
        out["faceContour"] = if (reverseJaw) jaw else jaw.reversed()
        out["leftEyebrow"] = band(listOf(22, 23, 24, 25, 26))
        out["rightEyebrow"] = band(listOf(17, 18, 19, 20, 21))
        out["leftEye"] = loop(pick((42 until 48).toList()), 8, 3)
        out["rightEye"] = loop(pick((36 until 42).toList()), 8, 5)
        out["nose"] = pick((31 until 36).toList())
        out["outerLips"] = loop(pick((48 until 60).toList()), 14, 4)
        out["innerLips"] = loop(pick((60 until 68).toList()), 6, 1)
        out[if (medianLine) "medianLine" else "noseCrest"] = CallFaceGeometry.resamplePolyline(pick(listOf(27, 28, 29, 30)), 6)
        return out.mapValues { (_, region) -> region.map { q -> q.values.map(::fine) } }
    }

    fun paint(ellipses: List<List<Double>>, width: Int, height: Int, scale: Double, originX: Double, originY: Double, salt: Int): CallFaceImage {
        val data = ByteArray(width * height * 4)
        for (py in 0 until height) {
            val wy = (py + 0.5) / scale + originY
            for (px in 0 until width) {
                val wx = (px + 0.5) / scale + originX
                val noise = ((px * 73856093) xor (py * 19349663) xor (salt * 83492791)) and 7
                var r = 196
                var g = 150
                var b = 126
                for (e in ellipses) {
                    val dx = wx - e[0]
                    val dy = wy - e[1]
                    if ((dx * dx) / (e[2] * e[2]) + (dy * dy) / (e[3] * e[3]) <= 1) {
                        r = e[4].toInt()
                        g = e[5].toInt()
                        b = e[6].toInt()
                    }
                }
                val at = (py * width + px) * 4
                data[at] = (r + noise).toByte()
                data[at + 1] = (g + noise).toByte()
                data[at + 2] = (b + noise).toByte()
                data[at + 3] = 255.toByte()
            }
        }
        return CallFaceImage(width, height, data)
    }

    fun anchorsOf(points: List<CallFacePoint>, yaw: Double?): CallFaceAnchors = CallFaceAnchors(
        eyeL = CallFaceGeometry.centroid(points, CallFaceGeometry.leftEye),
        eyeR = CallFaceGeometry.centroid(points, CallFaceGeometry.rightEye),
        mouth = CallFaceGeometry.centroid(points, listOf(48, 54, 51, 57)),
        mouthWidth = CallFaceJS.hypot(points[54].x - points[48].x, points[54].y - points[48].y),
        open = CallFaceGeometry.clampValue(CallFaceGeometry.mouthGap(points) * 3, 0.0, 1.0),
        yaw = yaw,
    )

    fun anchors(section: String, expected: Any?, a: CallFaceAnchors, label: String) {
        point(section, at(expected, "eyeL"), a.eyeL, "$label.eyeL")
        point(section, at(expected, "eyeR"), a.eyeR, "$label.eyeR")
        point(section, at(expected, "mouth"), a.mouth!!, "$label.mouth")
        close(section, at(expected, "open"), a.open!!, "$label.open")
        close(section, at(expected, "yaw"), a.yaw!!, "$label.yaw")
    }

    fun summary(): String =
        "call face parity: worst relative error " + worst.entries.joinToString(", ") { "${it.key}=${"%.2e".format(it.value)}" } +
            "; bit-identical hashes " + hashes.entries.joinToString(", ") { "${it.key} ${it.value.first}/${it.value.second}" }
}

@Suppress("UNCHECKED_CAST")
class CallFaceParityTest {
    private val p = CallFaceParity
    private val maya by lazy { CallFaces.head(CallFaces.byId("maya")!!) }

    private val staticPoses = listOf(
        Triple("neutral", CallFaceParity.Pose(), Triple(false, false, false)),
        Triple("turn-left", CallFaceParity.Pose(yaw = -0.45), Triple(false, false, false)),
        Triple("turn-right", CallFaceParity.Pose(yaw = 0.45), Triple(false, false, false)),
        Triple("tilted", CallFaceParity.Pose(roll = 0.3), Triple(false, false, false)),
        Triple("nodding", CallFaceParity.Pose(pitch = 0.25), Triple(false, false, false)),
        Triple("mouth-open", CallFaceParity.Pose(open = 1.0), Triple(false, false, false)),
        Triple("eyes-closed", CallFaceParity.Pose(close = doubleArrayOf(1.0, 1.0)), Triple(false, false, false)),
        Triple("wink-smile", CallFaceParity.Pose(close = doubleArrayOf(1.0, 0.0), smile = 1.0), Triple(false, false, false)),
        Triple("depth-turned", CallFaceParity.Pose(yaw = 0.3, pitch = -0.1), Triple(true, false, false)),
        Triple("depth-open", CallFaceParity.Pose(open = 0.6, roll = -0.2), Triple(true, false, false)),
        Triple("median-line", CallFaceParity.Pose(yaw = 0.1), Triple(false, true, true)),
        Triple("far-small", CallFaceParity.Pose(scale = 40.0, cx = 100.0, cy = 90.0, yaw = 0.2), Triple(false, false, false)),
    )

    fun testTheCataloguesMatchTheWeb() {
        val faces = p.list(p.fixture["faces"])
        assertEq(faces.size, CallFaces.all.size)
        faces.forEachIndexed { i, face ->
            val native = CallFaces.all[i]
            assertEq(p.at(face, "id"), native.id)
            assertEq(p.at(face, "label"), native.label)
            p.numbers("catalogue", p.at(face, "points"), native.points, "${native.id}.points")
            p.numbers("catalogue", p.at(face, "head"), native.head, "${native.id}.head")
        }
        val choices = p.list(p.at(p.fixture, "misc", "choices"))
        assertEq(choices.map { listOf(p.at(it, "id"), p.at(it, "label"), p.at(it, "custom")) }, CallFaces.choices().map { listOf(it.id, it.label, it.custom) })
        val outfits = p.list(p.at(p.fixture, "outfits", "catalogue"))
        assertEq(outfits.map { listOf(p.at(it, "id"), p.at(it, "label")) }, CallOutfits.all.map { listOf(it.id, it.label) })
        val constants = p.at(p.fixture, "outfits", "constants")
        assertEq(p.num(p.at(constants, "size")), CallOutfits.SIZE)
        assertEq(p.num(p.at(constants, "unit")), CallOutfits.UNIT)
        p.point("catalogue", p.at(constants, "neck"), CallOutfits.neck, "neck")
        assertEq(p.num(p.at(constants, "drop")), CallOutfits.DROP)
        assertEq(p.num(p.at(constants, "chinSpans")), CallOutfits.CHIN_SPANS)
        assertEq(p.num(p.at(constants, "roll")), CallOutfits.ROLL_SHARE)
        assertEq(p.int(p.at(constants, "tile")), CallOutfits.TILE)
        expect(CallOutfits.byId("nobody") == null)
        expect(CallFaces.byId("nobody") == null)
        expect(CallFaces.isCustom("custom") && !CallFaces.isCustom("maya"))
    }

    fun testTemplateAndFaceModelsMatch() {
        p.points("models", p.fixture["template"], CallFaces.template, "template")
        for (entry in p.list(p.fixture["faceModels"])) {
            val id = p.at(entry, "id") as String
            val face = CallFaces.byId(id)!!
            val head = CallFaces.head(face)
            p.points("models", p.at(entry, "canonical"), CallFaces.facePoints(face), "$id.canonical")
            p.points("models", p.at(entry, "layout"), head.layout, "$id.layout")
            p.points("models", p.at(entry, "model"), head.model, "$id.model")
            p.mesh(p.at(entry, "mesh"), head.mesh, id)
            p.list(p.at(entry, "eyes")).forEachIndexed { k, eye -> p.numbers("models", eye, CallFaceWarp.photoEyes(head.layout, 512.0)[k], "$id.eyes[$k]") }
        }
    }

    fun testDelaunayMatches() {
        val points = p.pointList(p.at(p.fixture, "delaunay", "points"))
        val expected = p.list(p.at(p.fixture, "delaunay", "triangles")).map { t -> p.list(t).map(p::int) }
        assertEq(expected, CallFaceGeometry.delaunay(points).map { it.toList() })
    }

    fun testMiscMatches() {
        val misc = p.fixture["misc"]
        val constants = p.at(misc, "constants")
        assertEq(p.int(p.at(constants, "landmarkCount")), CallFaceGeometry.LANDMARK_COUNT)
        assertEq(p.int(p.at(constants, "extraCount")), CallFaceGeometry.EXTRA_COUNT)
        assertEq(p.int(p.at(constants, "cheekCount")), CallFaceGeometry.CHEEK_COUNT)
        assertEq(p.int(p.at(constants, "mouthExtraCount")), CallFaceGeometry.MOUTH_EXTRA_COUNT)
        assertEq(p.num(p.at(constants, "foreheadLift")), CallFaceGeometry.FOREHEAD_LIFT)
        assertEq(p.int(p.at(constants, "vertexFloats")), CallFaceWarp.VERTEX_FLOATS)
        for (row in p.list(p.at(misc, "outputSize"))) {
            val args = p.nums(p.at(row, 0))
            val size = if (args.size > 2) CallFaces.outputSize(args[0], args[1], args[2]) else CallFaces.outputSize(args[0], args[1])
            assertEq(p.nums(p.at(row, 1)), listOf(size.first.toDouble(), size.second.toDouble()), "outputSize $args")
        }
        assertEq(listOf(true, false), p.list(p.at(misc, "custom")))
        val regions = mapOf("leftPupil" to listOf(listOf(300.0, 200.0)), "rightPupil" to listOf(listOf(200.0, 201.0)))
        p.points("misc", p.at(misc, "pupils", 0), CallFaces.pupils(regions, 2.0)!!, "pupils")
        expect(CallFaces.pupils(mapOf("leftPupil" to listOf(listOf(300.0, 200.0)))) == null)
        val host = CallFaces.located(mapOf("faceContour" to listOf(listOf(0.0, 0.0))), 1.0, true)
        expect(host.points == null && host.pupils == null)
        for (row in p.list(p.at(misc, "vertexRegion"))) {
            val v = p.int(p.at(row, 0))
            p.close("misc", p.at(row, 1), CallFaceWarp.vertexRegion(v), "vertexRegion $v")
            p.close("misc", p.at(row, 2), CallFaceWarp.vertexRegion(v, 150), "vertexRegion $v/150")
        }
        for (row in p.list(p.at(misc, "coverRect"))) {
            val a = p.nums(p.at(row, 0))
            p.numbers("misc", p.at(row, 1), CallFaceWarp.coverRect(a[0], a[1], a[2], a[3]), "coverRect $a")
        }
        val looks = listOf(null, listOf(CallFacePoint(0.1, -0.2), CallFacePoint(0.3, 0.4)), listOf(CallFacePoint(0.1, Double.NaN), null))
        looks.forEachIndexed { i, look -> p.numbers("misc", p.at(misc, "lookUniform", i), CallFaceWarp.lookUniform(look), "lookUniform $i") }
        p.axis("misc", p.at(misc, "axisFrame"), CallFaceWarp.axisFrame(CallFaceBox(CallFacePoint(10.0, 20.0), CallFacePoint(0.6, 0.8), CallFacePoint(-0.8, 0.6), CallFacePoint(4.0, 5.0))), "axisFrame")
        p.close("misc", p.at(misc, "luma"), CallFaceExpression.lumaOf(200.0, 100.0, 50.0), "luma")
        p.close("misc", p.at(misc, "redness"), CallFaceExpression.rednessOf(200.0, 100.0, 50.0), "redness")
        p.numbers("misc", p.at(misc, "pairClosure"), CallFaceExpression.pairClosure(doubleArrayOf(0.9, 0.2)), "pairClosure")
        p.points("misc", p.at(misc, "closeNone"), CallFaceExpression.closeLids(listOf(CallFacePoint(1.0, 2.0)), doubleArrayOf(0.0, 0.005)), "closeNone")
        expect(CallFaceGeometry.landmarksFromRegions(mapOf("faceContour" to listOf(listOf(0.0, 0.0)))) == null)
        val neutral = p.regions(p.at(p.fixture, "statics", 0, "regions"))!!.toMutableMap()
        neutral["noseCrest"] = listOf(listOf(1.0, 2.0))
        expect(CallFaceGeometry.landmarksFromRegions(neutral) == null)
        p.points("misc", p.at(misc, "resample", 0), CallFaceGeometry.resamplePolyline(listOf(CallFacePoint(1.0, 2.0, 3.0)), 3), "resample0")
        p.points("misc", p.at(misc, "resample", 1), CallFaceGeometry.resamplePolyline(listOf(CallFacePoint(0.0, 0.0), CallFacePoint(3.0, 4.0), CallFacePoint(3.0, 4.0), CallFacePoint(6.0, 8.0)), 5), "resample1")
        assertEq(p.int(p.at(misc, "ringCount")), CallFaceGeometry.ringCount(maya.model.size))
    }

    fun testSyntheticRegionsMatchTheGenerator() {
        val statics = p.list(p.fixture["statics"])
        staticPoses.forEachIndexed { i, (name, pose, flags) ->
            assertEq(name, p.at(statics[i], "name"))
            val regions = p.visionRegions(p.synthetic(pose), flags.first, flags.second, flags.third)
            val stored = p.regions(p.at(statics[i], "regions"))!!
            assertEq(stored.keys, regions.keys, "$name keys")
            for ((key, region) in stored) {
                assertEq(region.size, regions[key]!!.size, "$name.$key")
                region.forEachIndexed { k, q -> p.numbers("generator", q, regions[key]!![k], "$name.$key[$k]") }
            }
        }
    }

    fun testStaticCasesMatch() {
        val template = CallFaces.template
        for (case in p.list(p.fixture["statics"])) {
            val name = p.at(case, "name") as String
            val s = "statics"
            val points = CallFaceGeometry.landmarksFromRegions(p.regions(p.at(case, "regions")))!!
            p.points(s, p.at(case, "points"), points, "$name.points")
            p.frame(s, p.at(case, "frame"), CallFaceGeometry.faceFrame(points), "$name.frame")
            p.points(s, p.at(case, "normalized"), CallFaceGeometry.normalizeShape(points), "$name.normalized")
            p.points(s, p.at(case, "canonical"), CallFaceGeometry.canonicalShape(points), "$name.canonical")
            p.close(s, p.at(case, "mouthGap"), CallFaceGeometry.mouthGap(points), "$name.mouthGap")
            p.close(s, p.at(case, "noseTurn"), CallFaceGeometry.noseTurn(points), "$name.noseTurn")
            assertEq(p.at(case, "hasDepth"), CallFaceGeometry.hasDepth(points), "$name.hasDepth")
            p.pose(s, p.at(case, "pose"), CallFaceGeometry.solvePose(maya.model, points), "$name.pose")
            val head = CallFaceGeometry.liveHead(maya.model, points)
            p.head(s, p.at(case, "head"), head, maya, points, 2, "$name.head")
            p.box(s, p.at(case, "eyeBoxes", 0), CallFaceExpression.eyeBox(points, 0), "$name.eyeBox0")
            p.box(s, p.at(case, "eyeBoxes", 1), CallFaceExpression.eyeBox(points, 1), "$name.eyeBox1")
            p.close(s, p.at(case, "eyeOpenness", 0), CallFaceExpression.eyeOpenness(points, 0), "$name.open0")
            p.close(s, p.at(case, "eyeOpenness", 1), CallFaceExpression.eyeOpenness(points, 1), "$name.open1")
            p.box(s, p.at(case, "tongueWindow"), CallFaceExpression.tongueWindow(points), "$name.tongue")
            p.points(s, p.at(case, "closed"), CallFaceExpression.closeLids(points, doubleArrayOf(0.8, 0.3)).subList(36, 48), "$name.closed")
            val outfit = CallOutfits.frame(points)
            p.outfit(s, p.at(case, "outfit"), outfit, "$name.outfit")
            p.point(s, p.at(case, "outfitUv"), CallOutfits.uv(outfit!!, CallFacePoint(320.0, 400.0)), "$name.outfitUv")
            p.points(s, p.at(case, "fit"), CallFaceGeometry.fitTemplate(template, p.anchorsOf(points, head.pose.yaw)), "$name.fit")
            val plain = CallFaceAnchors(eyeL = CallFaceGeometry.centroid(points, CallFaceGeometry.leftEye), eyeR = CallFaceGeometry.centroid(points, CallFaceGeometry.rightEye))
            p.points(s, p.at(case, "fitPlain"), CallFaceGeometry.fitTemplate(template, plain), "$name.fitPlain")
        }
    }

    fun testEveryFaceFollowsTheHead() {
        val neutral = CallFaceGeometry.landmarksFromRegions(p.visionRegions(p.synthetic(CallFaceParity.Pose(yaw = 0.2))))!!
        val depth = CallFaceGeometry.landmarksFromRegions(p.visionRegions(p.synthetic(CallFaceParity.Pose(yaw = -0.25, pitch = 0.1)), depth = true))!!
        for (entry in p.list(p.fixture["everyFace"])) {
            val id = p.at(entry, "id") as String
            val head = CallFaces.head(CallFaces.byId(id)!!)
            p.head("everyFace", p.at(entry, "flat"), CallFaceGeometry.liveHead(head.model, neutral), head, neutral, 4, "$id.flat")
            p.head("everyFace", p.at(entry, "depth"), CallFaceGeometry.liveHead(head.model, depth), head, depth, 4, "$id.depth")
        }
    }

    fun testTheSequenceMatchesTheStagePipeline() {
        val sequence = p.fixture["sequence"]
        val scale = p.num(p.at(sequence, "scale"))
        val frames = p.list(p.at(sequence, "frames"))
        val outputs = p.list(p.at(sequence, "outputs"))
        val probe = CallFaceExpressionProbe()
        val pointSmoother = CallFaceLandmarkSmoother()
        val anchorSmoother = CallFaceAnchorSmoother()
        val outfitSmoother = CallOutfitSmoother()
        val template = CallFaces.template
        var live: List<CallFacePoint>? = null
        var expression: CallFaceExpressionReading? = null
        val s = "sequence"
        frames.forEachIndexed { k, frame ->
            val out = outputs[k]
            val time = p.num(p.at(frame, "time"))
            val regions = p.regions(p.at(frame, "regions"))
            val found = CallFaces.located(regions, scale, true)
            val stored = p.at(out, "found")
            if (stored == null) expect(found.points == null, "frame $k found") else p.point(s, stored, found.points!![30], "$k.found")
            if (p.at(out, "pupils") == null) expect(found.pupils == null, "frame $k pupils") else p.points(s, p.at(out, "pupils"), found.pupils!!, "$k.pupils")
            val points = found.points?.let { pointSmoother.smooth(it, time) }
            if (points == null) expect(p.at(out, "points") == null, "frame $k points") else p.points(s, p.at(out, "points"), points, "$k.points")
            if (points != null) live = points
            if (points != null) {
                val crop = CallFaceExpression.probeCrop(points)
                val stored = p.at(out, "crop")
                assertEq(p.int(p.at(stored, "width")), crop.width, "$k crop width")
                assertEq(p.int(p.at(stored, "height")), crop.height, "$k crop height")
                p.close(s, p.at(stored, "scale"), crop.scale, "$k crop scale")
                p.numbers(s, p.at(stored, "origin"), doubleArrayOf(crop.minX, crop.minY), "$k crop origin")
                val paintOrigin = p.nums(p.at(stored, "paintOrigin"))
                val ellipses = p.list(p.at(frame, "ellipses")).map(p::nums)
                val image = p.paint(ellipses, p.int(p.at(stored, "width")), p.int(p.at(stored, "height")), p.num(p.at(stored, "scale")), paintOrigin[0], paintOrigin[1], k)
                expression = probe.measure(image, crop, points, time, found.pupils, true) ?: expression
            }
            val reading = expression
            val e = p.at(out, "expression")
            if (e == null) {
                expect(reading == null, "frame $k expression")
            } else {
                p.numbers(s, p.at(e, "close"), reading!!.close, "$k.close")
                p.points(s, p.at(e, "look"), reading.look, "$k.look")
                p.close(s, p.at(e, "tongue"), reading.tongue, "$k.tongue")
                p.close(s, p.at(e, "skinRed"), reading.skinRed, "$k.skinRed")
                p.close(s, p.at(e, "skinLuma"), reading.skinLuma, "$k.skinLuma")
            }
            val held = live
            val drawn = if (held != null && reading != null) CallFaceExpression.closeLids(held, reading.close) else held
            if (drawn != null) {
                p.points(s, p.at(out, "drawn"), drawn.subList(36, 48), "$k.drawn")
                val head = CallFaceGeometry.liveHead(maya.model, drawn)
                val depths = CallFaceGeometry.vertexDepths(maya.model, head.pose)
                p.pose(s, p.at(out, "pose"), head.pose, "$k.pose")
                p.points(s, p.at(out, "head"), p.strided(head.points, 5), "$k.head")
                p.numbers(s, p.at(out, "depths"), p.stridedArray(depths, 7), "$k.depths")
                p.floats("$s.warp", p.at(out, "warp"), CallFaceWarp.warpVertices(maya.mesh, maya.layout, head.points, 512.0, depths), 211, "$k.warp")
                p.box(s, p.at(out, "tongueBox"), CallFaceExpression.tongueWindow(drawn), "$k.tongueBox")
            }
            p.outfit(s, p.at(out, "outfit"), outfitSmoother.smooth(if (held != null) CallOutfits.frame(held) else null), "$k.outfit")
            if (points != null) {
                val smooth = anchorSmoother.smooth(p.anchorsOf(points, if (k % 9 == 4) null else CallFaceGeometry.noseTurn(points)), time)
                p.anchors(s, p.at(out, "anchors"), smooth, "$k.anchors")
                p.points(s, p.at(out, "fit"), CallFaceGeometry.fitTemplate(template, smooth), "$k.fit")
            }
        }
    }

    fun testFiltersMatch() {
        val filters = p.fixture["filters"]
        val inputs = p.list(p.at(filters, "oneEuro", "inputs")).map(p::nums)
        val outputs = p.list(p.at(filters, "oneEuro", "outputs"))
        val resetAt = p.int(p.at(filters, "oneEuro", "resetAt"))
        val plain = CallFaceOneEuroFilter()
        val custom = CallFaceOneEuroFilter(0.6, 0.3, 2.0)
        inputs.forEachIndexed { k, (value, time, scale) ->
            if (k == resetAt) {
                plain.reset()
                custom.reset()
            }
            p.numbers("filters", outputs[k], doubleArrayOf(plain.filter(value, time, scale), custom.filter(value, time, scale)), "oneEuro $k")
        }
        val blink = CallFaceBlinkTracker()
        val blinkOutputs = p.list(p.at(filters, "blink", "outputs"))
        p.list(p.at(filters, "blink", "inputs")).forEachIndexed { k, input ->
            val edges = p.list(p.at(input, "edges")).map { if (it == null) Double.NaN else p.num(it) }.toDoubleArray()
            val openness = p.at(input, "openness")?.let { p.nums(it).toDoubleArray() }
            val result = blink.update(CallFaceBlinkInput(edges, openness, p.num(p.at(input, "elapsed")), p.num(p.at(input, "time")), p.at(input, "moving") as Boolean))
            p.numbers("filters", blinkOutputs[k], result, "blink $k")
        }
    }

    fun testDetectionDecodingMatches() {
        val d = p.fixture["detection"]
        val next = p.xorshift(p.num(p.at(d, "seed")).toLong())
        val anchors = CallFaceMesh.blazeAnchors()
        assertEq(p.int(p.at(d, "anchorCount")), anchors.size)
        val scores = FloatArray(anchors.size)
        val boxes = FloatArray(anchors.size * 16)
        val keys = listOf(0.43 to 0.4, 0.57 to 0.4, 0.5 to 0.5, 0.5 to 0.58, 0.36 to 0.45, 0.64 to 0.45)
        anchors.forEachIndexed { i, anchor ->
            val ax = anchor.x
            val ay = anchor.y
            val near = Math.abs(ax - 0.5) < 0.07 && Math.abs(ay - 0.47) < 0.07
            scores[i] = (if (near) 1 + 3 * next() else -9 + 6 * next()).toFloat()
            val r = i * 16
            boxes[r] = ((0.5 - ax) * 128 + (next() - 0.5) * 3).toFloat()
            boxes[r + 1] = ((0.47 - ay) * 128 + (next() - 0.5) * 3).toFloat()
            boxes[r + 2] = (56 + 8 * next()).toFloat()
            boxes[r + 3] = (60 + 8 * next()).toFloat()
            keys.forEachIndexed { k, (kx, ky) ->
                boxes[r + 4 + k * 2] = ((kx - ax) * 128 + (next() - 0.5) * 2).toFloat()
                boxes[r + 5 + k * 2] = ((ky - ay) * 128 + (next() - 0.5) * 2).toFloat()
            }
        }
        p.points("detection", p.at(d, "anchors"), p.strided(anchors, 37), "anchors")
        p.numbers("detection", p.at(d, "scores"), p.stridedArray(scores.map { it.toDouble() }.toDoubleArray(), 41), "scores")
        p.numbers("detection", p.at(d, "boxes"), p.stridedArray(boxes.map { it.toDouble() }.toDoubleArray(), 113), "boxes")
        val decoded = CallFaceMesh.decodeDetections(scores, boxes, anchors)!!
        val e = p.at(d, "decoded")
        for ((key, value) in listOf("score" to decoded.score, "cx" to decoded.cx, "cy" to decoded.cy, "w" to decoded.w, "h" to decoded.h)) p.close("detection", p.at(e, key), value, key)
        p.points("detection", p.at(e, "keypoints"), decoded.keypoints, "keypoints")
        expect(CallFaceMesh.decodeDetections(FloatArray(anchors.size) { -5f }, boxes, anchors) == null)
        val roi = CallFaceMesh.roiFromDetection(decoded, CallFaceLetterbox(640.0, 0.0, (640.0 - 480.0) / 2 / 640))
        val r = p.at(d, "roi")
        p.numbers("detection", listOf(p.at(r, "cx"), p.at(r, "cy"), p.at(r, "size"), p.at(r, "angle")), doubleArrayOf(roi.cx, roi.cy, roi.size, roi.angle), "roi")
        assertEq(CallFaceLetterbox(640.0, 0.0, (640.0 - 480.0) / 2 / 640), CallFaceLetterbox.of(640.0, 480.0))
    }

    fun testFaceMeshRegionsMatch() {
        val m = p.fixture["faceMesh"]
        val mesh = p.pointList(p.at(m, "mesh"))
        val regions = CallFaceMesh.meshRegions(mesh)
        val stored = p.regions(p.at(m, "regions"))!!
        assertEq(stored.keys, regions.keys)
        for ((key, region) in stored) region.forEachIndexed { k, q -> p.numbers("faceMesh", q, regions[key]!![k], "$key[$k]") }
        p.points("faceMesh", p.at(m, "landmarks"), CallFaceMesh.meshToLandmarks(mesh)!!, "landmarks")
        p.points("faceMesh", p.at(m, "flat"), CallFaceMesh.meshToLandmarks(mesh.map { it.flat })!!, "flat")
        val roi = CallFaceMesh.roiFromMesh(mesh)
        val r = p.at(m, "roi")
        p.numbers("faceMesh", listOf(p.at(r, "cx"), p.at(r, "cy"), p.at(r, "size"), p.at(r, "angle")), doubleArrayOf(roi.cx, roi.cy, roi.size, roi.angle), "roi")
        val points = CallFaceMesh.meshPoints(floatArrayOf(96f, 96f, 19.2f, 0f, 192f, 0f), CallFaceRoi(100.0, 50.0, 192.0, 0.0))
        p.points("faceMesh", listOf(listOf(100.0, 50.0, 19.2f.toDouble()), listOf(4.0, 146.0, 0.0)), points, "meshPoints")
    }

    fun testCustomFaceMatteMatches() {
        val c = p.fixture["custom"]
        val size = p.int(p.at(c, "size"))
        val raw = p.pointList(p.at(c, "raw"))
        val transform = CallFaceCustom.transform(raw)
        p.close("custom", p.at(c, "transform", "scale"), transform.scale, "transform.scale")
        p.numbers("custom", p.at(c, "transform", "matrix"), transform.matrix, "transform.matrix")
        val placed = raw.map { CallFaceCustom.apply(transform.matrix, it) }
        p.points("custom", p.at(c, "placed"), placed, "placed")
        val headStored = p.nums(p.at(c, "results", "plain", "head"))
        p.numbers("custom", p.at(c, "flat"), CallFaceCustom.face(placed, headStored).points, "flat")
        for (row in p.list(p.at(c, "smoothstep"))) {
            val v = p.nums(row)
            p.close("custom", v[3], CallFaceCustom.smoothstep(v[0], v[1], v[2]), "smoothstep $v")
        }
        val blurIn = p.nums(p.at(c, "blur", "input")).map { it.toFloat() }.toFloatArray()
        p.numbers("custom", p.at(c, "blur", "output"), CallFaceCustom.boxBlur(blurIn, 8, 2, 2).map { it.toDouble() }.toDoubleArray(), "blur")
        val pixels = ByteArray(size * size * 4)
        for (y in 0 until size) {
            for (x in 0 until size) {
                val at = (y * size + x) * 4
                val dx = (x - 256).toDouble()
                val dy = (y - 300).toDouble()
                val skin = dx * dx / (150.0 * 150.0) + dy * dy / (190.0 * 190.0) <= 1
                val hair = !skin && dx * dx / (190.0 * 190.0) + (dy + 60) * (dy + 60) / (220.0 * 220.0) <= 1
                val noise = ((x * -1640531535) xor (y * 40503)) ushr 27
                val base = if (skin) intArrayOf(204, 152, 128) else if (hair) intArrayOf(52, 38, 30) else intArrayOf(30 + (x shr 3), 90, 140 - (y shr 3))
                pixels[at] = (base[0] + noise).toByte()
                pixels[at + 1] = (base[1] + noise).toByte()
                pixels[at + 2] = (base[2] + noise).toByte()
                pixels[at + 3] = 255.toByte()
            }
        }
        val personBytes = ByteArray(size * size)
        for (y in 0 until size) {
            for (x in 0 until size) {
                val dx = (x - 256).toDouble()
                val dy = (y - 280).toDouble()
                personBytes[y * size + x] = (if (dx * dx / (210.0 * 210.0) + dy * dy / (300.0 * 300.0) <= 1) 250 else ((x * 7 + y * 3) and 31)).toByte()
            }
        }
        val person = CallFaceCustom.personWeights(personBytes)
        p.floats("custom.matte", p.at(c, "personSamples"), person, 997, "person")
        val span = CallFaceGeometry.faceFrame(placed).span
        val model = CallFaceCustom.skinModel(pixels, size, placed, span)!!
        p.numbers("custom", p.at(c, "skinModel", "mean"), model.mean, "skinModel.mean")
        p.numbers("custom", p.at(c, "skinModel", "inverse"), model.inverse, "skinModel.inverse")
        p.floats("custom.matte", p.at(c, "likeness"), CallFaceCustom.skinLikeness(pixels, size, model), 997, "likeness")
        val polygons = CallFaceCustom.polygons(placed)
        val face = CallFaceCustom.rasterize(polygons.face, size)
        val inner = CallFaceCustom.rasterize(polygons.inner, size)
        val neck = CallFaceCustom.rasterize(polygons.neck, size)
        for ((name, mask) in listOf("plain" to null, "person" to person)) {
            val e = p.at(c, "results", name)
            assertEq(p.list(p.at(e, "polygons")).map(p::int), listOf(polygons.face.size, polygons.inner.size, polygons.neck.size), "$name polygons")
            val built = CallFaceCustom.buildMatte(pixels, size, placed, mask, face, inner, neck)
            p.floats("custom.matte", p.at(e, "matte"), built.matte, 997, "$name.matte")
            p.floats("custom.matte", p.at(e, "skin"), built.skin, 997, "$name.skin")
            p.floats("custom.matte", p.at(e, "hair"), built.hair, 997, "$name.hair")
            p.numbers("custom", p.at(e, "head"), built.head, "$name.head")
        }
        val built = CallFaces.head(CallFaceModel(CallFaceCustom.ID, CallFaceCustom.LABEL, p.nums(p.at(c, "flat")), headStored))
        p.points("custom", p.at(c, "faceModel", "layout"), built.layout, "faceModel.layout")
        p.points("custom", p.at(c, "faceModel", "model"), built.model, "faceModel.model")
        p.mesh(p.at(c, "faceModel", "mesh"), built.mesh, "faceModel")
        assertEq("face_not_found", runCatching { CallFaceCustom.validate(null) }.exceptionOrNull()?.message)
    }

    private fun style(paint: CallOutfitPaint, fabrics: MutableList<CallOutfitFabric>): Any? = when (paint) {
        is CallOutfitPaint.Color -> paint.color
        is CallOutfitPaint.Fabric -> {
            if (paint.fabric !in fabrics) fabrics.add(paint.fabric)
            "pattern:${fabrics.indexOf(paint.fabric)}"
        }
        is CallOutfitPaint.Gradient -> paint.gradient
    }

    private fun expectedStyle(value: Any?): Any? = when (value) {
        is String -> CallOutfitColor.css(value)
        else -> {
            val map = p.obj(value)
            if (map.containsKey("pattern")) "pattern:${p.int(map["pattern"])}" else {
                val g = p.nums(map["gradient"])
                CallOutfitGradient(g[0], g[1], g[2], g[3], p.list(map["stops"]).map { CallOutfitGradientStop(p.num(p.at(it, 0)), CallOutfitColor.css(p.at(it, 1) as String)) })
            }
        }
    }

    private fun describe(command: CallOutfitCommand, fabrics: MutableList<CallOutfitFabric>): Pair<String, List<Any?>> = when (command) {
        CallOutfitCommand.Save -> "save" to emptyList()
        CallOutfitCommand.Restore -> "restore" to emptyList()
        CallOutfitCommand.BeginPath -> "beginPath" to emptyList()
        CallOutfitCommand.ClosePath -> "closePath" to emptyList()
        CallOutfitCommand.Fill -> "fill" to emptyList()
        CallOutfitCommand.Stroke -> "stroke" to emptyList()
        CallOutfitCommand.Clip -> "clip" to emptyList()
        is CallOutfitCommand.MoveTo -> "moveTo" to listOf(command.x, command.y)
        is CallOutfitCommand.LineTo -> "lineTo" to listOf(command.x, command.y)
        is CallOutfitCommand.QuadTo -> "quadTo" to listOf(command.cx, command.cy, command.x, command.y)
        is CallOutfitCommand.Arc -> "arc" to listOf(command.x, command.y, command.radius, command.start, command.end)
        is CallOutfitCommand.FillRect -> "fillRect" to listOf(command.x, command.y, command.w, command.h)
        is CallOutfitCommand.Translate -> "translate" to listOf(command.x, command.y)
        is CallOutfitCommand.Rotate -> "rotate" to listOf(command.angle)
        is CallOutfitCommand.FillStyle -> "fillStyle" to listOf(style(command.paint, fabrics))
        is CallOutfitCommand.StrokeStyle -> "strokeStyle" to listOf(style(command.paint, fabrics))
        is CallOutfitCommand.LineWidth -> "lineWidth" to listOf(command.width)
        is CallOutfitCommand.LineCap -> "lineCap" to listOf(command.cap)
        is CallOutfitCommand.LineJoin -> "lineJoin" to listOf(command.join)
        is CallOutfitCommand.LineDash -> "lineDash" to listOf(command.dash)
        is CallOutfitCommand.Composite -> "composite" to listOf(command.operation)
    }

    private fun sameValue(expected: Any?, actual: Any?, label: String) {
        when (actual) {
            is Double -> p.close("outfits", expected, actual, label)
            is List<*> -> {
                val e = p.list(expected)
                assertEq(e.size, actual.size, label)
                e.forEachIndexed { i, v -> sameValue(v, actual[i], "$label[$i]") }
            }
            is CallOutfitColor -> {
                val c = expectedStyle(expected) as CallOutfitColor
                p.numbers("outfits", listOf(c.r, c.g, c.b, c.a), doubleArrayOf(actual.r, actual.g, actual.b, actual.a), label)
            }
            is CallOutfitGradient -> {
                val g = expectedStyle(expected) as CallOutfitGradient
                p.numbers("outfits", listOf(g.x0, g.y0, g.x1, g.y1), doubleArrayOf(actual.x0, actual.y0, actual.x1, actual.y1), label)
                assertEq(g.stops, actual.stops, label)
            }
            is String -> assertEq(if (actual.startsWith("pattern:")) expectedStyle(expected) else expected, actual, label)
            else -> assertEq(expected, actual, label)
        }
    }

    fun testOutfitDrawingsMatchTheRecordedCanvas() {
        val fabrics = ArrayList<CallOutfitFabric>()
        for (drawing in p.list(p.at(p.fixture, "outfits", "drawings"))) {
            val id = p.at(drawing, "id") as String
            val ops = p.list(p.at(drawing, "ops"))
            val commands = CallOutfitDrawing.commands(id)!!
            assertEq(ops.size, commands.size, "$id op count")
            ops.forEachIndexed { i, op ->
                val e = p.list(op)
                val (name, args) = describe(commands[i], fabrics)
                assertEq(e[0], name, "$id op $i")
                assertEq(e.size - 1, args.size, "$id op $i args")
                args.forEachIndexed { k, arg -> sameValue(e[k + 1], arg, "$id op $i ($name) arg $k") }
            }
        }
        expect(CallOutfitDrawing.commands("") == null)
        val tiles = p.list(p.at(p.fixture, "outfits", "tiles"))
        assertEq(tiles.size, fabrics.size, "tile count")
        var worstByte = 0
        tiles.forEachIndexed { i, tile ->
            val data = CallOutfitDrawing.fabricTile(fabrics[i])
            p.nums(p.at(tile, "samples")).forEachIndexed { k, v ->
                val diff = Math.abs((data[k * 257].toInt() and 0xff) - v.toInt())
                worstByte = maxOf(worstByte, diff)
                expect(diff <= 1, "tile $i byte ${k * 257}: expected $v")
            }
            p.countHash("tiles", p.num(p.at(tile, "hash")).toLong() == p.fnvBytes(data))
        }
        p.worst["tileBytes"] = worstByte.toDouble()
    }

    fun testZzzPrintSummary() {
        println(p.summary())
    }
}
